#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/time.h>

#include "../../kv_app.h"
#include "../../kv_data_store.h"
#include "../../kv_memory.h"
#include "../../kv_msg.h"
#include "../../kv_ring.h"
#include "../../utils/ditto_wrapper.h"
#include "../../utils/timing.h"
#include "../../utils/uthash.h"
struct {
    uint32_t ssd_num;
    uint32_t worker_num;
    uint32_t storage_stride;
    uint32_t thread_num;
    uint32_t concurrent_io_num, copy_concurrency;
    uint32_t set_batch;
    uint32_t ring_num, vid_per_ssd, rpl_num;
    char json_config_file[1024];
    char server_conf_file[1024];
    char etcd_ip[32];
    char etcd_port[16];
    char local_ip[32];
    char local_port[16];
    bool ditto;
    bool ours;

} opt = {.ssd_num = 4,
         .worker_num = 4,
         .storage_stride = 4,
         .thread_num = 3,
         .concurrent_io_num = 2048,
         .copy_concurrency = 32,
         .set_batch = 1,
         .ring_num = 128,
         .vid_per_ssd = 128,
         .rpl_num = 1,
         .json_config_file = "server.config.json",
         .server_conf_file = "app/leed/ditto/experiments/configs/server_conf_sample.json",
         .ditto = false,
         .ours = false,
         .etcd_ip = "127.0.0.1",
         .etcd_port = "2379",
         .local_ip = "10.3.4.6",
         .local_port = "9000"};

static void help(void) {
    printf("Program options:\n");
    printf("  -h               Display this help message\n");
    printf("  -d <ssd_num>     Set the number of SSDs: %u\n", opt.ssd_num);
    printf("  -S <stride>      Set the stride of storage workers: %u\n", opt.storage_stride);
    printf("  -c <config_file> Set the SPDK JSON config file: %s\n", opt.json_config_file);
    printf("  -f <server_conf> Set the Ditto server config file: %s\n", opt.server_conf_file);
    printf("  -i <io_num>      Set the maximum number of concurrent I/Os: %u\n", opt.concurrent_io_num);
    printf("  -I <copy_concur> Set the copy concurrency: %u\n", opt.copy_concurrency);
    printf("  -b <set_batch>   Set the batch size of set buffers: %u\n", opt.set_batch);
    printf("  -T <thread_num>  Set the number of threads for handling RDMA requests: %u\n", opt.thread_num);
    printf("  -s <etcd_ip>     Set the etcd's IP: %s\n", opt.etcd_ip);
    printf("  -P <etcd_port>   Set the etcd's port: %s\n", opt.etcd_port);
    printf("  -l <local_ip>    Set the local IP for remote connects: %s\n", opt.local_ip);
    printf("  -p <local_port>  Set the local port for remote connects: %s\n", opt.local_port);
    printf("  -m <vid_per_ssd> Set the number of VID per SSD (must be same within a cluster): %u\n", opt.vid_per_ssd);
    printf("  -R <rpl_num>     Set the number of replica (must be same within a cluster): %u\n", opt.rpl_num);
    printf("  -C <ditto/ours>  Enable caching\n");
}

static void get_options(int argc, char **argv) {
    int ch;
    while ((ch = getopt(argc, argv, "hr:d:S:c:f:i:T:s:P:l:p:m:R:I:b:C:")) != -1) switch (ch) {
            case 'd':
                opt.ssd_num = atol(optarg);
                break;
            case 'S':
                opt.storage_stride = atol(optarg);
                break;
            case 'c':
                strcpy(opt.json_config_file, optarg);
                break;
            case 'f':
                strcpy(opt.server_conf_file, optarg);
                break;
            case 'i':
                opt.concurrent_io_num = atol(optarg);
                break;
            case 'I':
                opt.copy_concurrency = atol(optarg);
                break;
            case 'b':
                opt.set_batch = atol(optarg);
                break;
            case 'T':
                opt.thread_num = atol(optarg);
                break;
            case 's':
                strcpy(opt.etcd_ip, optarg);
                break;
            case 'P':
                strcpy(opt.etcd_port, optarg);
                break;
            case 'l':
                strcpy(opt.local_ip, optarg);
                break;
            case 'p':
                strcpy(opt.local_port, optarg);
                break;
            case 'm':
                opt.vid_per_ssd = atol(optarg);
                break;
            case 'R':
                opt.rpl_num = atol(optarg);
                break;
            case 'C':
                if (strcmp(optarg, "ditto") == 0) {
                    opt.ditto = true;
                } else if (strcmp(optarg, "ours") == 0) {
                    opt.ours = true;
                } else {
                    help();
                    exit(-1);
                }
                break;
            default:
                help();
                exit(-1);
        }
}

#define MAX_SSD_WORKERS 4
#define MAX_STORAGE_STRIDE 8
#define SET_CTX_BUFFER_SIZE 128

struct io_ctx;

struct set_buffer {
    uint8_t *key[SET_CTX_BUFFER_SIZE];
    uint8_t key_length[SET_CTX_BUFFER_SIZE];
    uint8_t *value[SET_CTX_BUFFER_SIZE];
    uint32_t value_length[SET_CTX_BUFFER_SIZE];
    struct io_ctx *io[SET_CTX_BUFFER_SIZE];
    uint32_t buffer_size;
    struct timeval active;
};

struct worker_t {
    struct kv_storage storage[MAX_STORAGE_STRIDE];
    struct kv_data_store data_store[MAX_STORAGE_STRIDE];
    struct set_buffer set_buffer[MAX_STORAGE_STRIDE];
} * workers;

kv_rdma_handle server;
kv_rdma_mrs_handle server_mrs;

struct io_ctx {
    void *req_h;
    struct kv_msg *msg;
    uint32_t worker_id;
    uint32_t storage_id;
    uint32_t server_thread;
    kv_rdma_mr req;
    void *fwd_ctx;
    bool has_next_node, need_forward, in_copy_pool;
    uint32_t vnode_type;
    kv_data_store_ctx ds_ctx;
    uint32_t msg_type;
    struct kv_data_store_copy_buf *copy_buf;
    uint8_t *key[SET_CTX_BUFFER_SIZE];
    uint8_t key_length[SET_CTX_BUFFER_SIZE];
    uint8_t *value[SET_CTX_BUFFER_SIZE];
    uint32_t value_length[SET_CTX_BUFFER_SIZE];
    struct io_ctx *io[SET_CTX_BUFFER_SIZE];
    uint64_t value_offset[SET_CTX_BUFFER_SIZE];
    uint64_t bucket_id[SET_CTX_BUFFER_SIZE];
    struct kv_bucket_segment seg[SET_CTX_BUFFER_SIZE];
    uint32_t buffer_size;
};

struct kv_mempool *io_pool, *copy_pool;
struct kv_ds_queue ds_queue;

static void *buf_poller = NULL;

// #define WORKER_INDEX ((self - workers) * opt.storage_stride + i)
#define WORKER_INDEX (i * MAX_SSD_WORKERS + (self - workers))

static void worker_stop(void *arg) {
    struct worker_t *self = arg;
    for (uint32_t i = 0; i < MAX_STORAGE_STRIDE; ++i) {
        if (WORKER_INDEX >= opt.ssd_num) {
            break;
        }
        kv_data_store_fini(&self->data_store[i]);
        kv_storage_fini(&self->storage[i]);
    }
    kv_app_stop(0);
}
static void thread_stop(void *arg) { kv_app_stop(0); }
static void ring_fini_cb(void *arg) {
    for (size_t i = 0; i < opt.worker_num; i++) kv_app_send(i, worker_stop, workers + i);
    for (size_t i = 0; i < opt.thread_num; i++) kv_app_send(opt.worker_num + i, thread_stop, NULL);
}

static void send_response(void *arg) {
    struct io_ctx *io = arg;
    io->msg->q_info = ds_queue.q_info[io->worker_id];
    kv_rdma_make_resp(io->req_h, (uint8_t *)io->msg, KV_MSG_SIZE(io->msg));
    kv_mempool_put(io_pool, io);
}

static void buffered_send_response(void *arg) {
    struct io_ctx *io = arg;
    for (uint32_t i = 0; i < io->buffer_size; ++i) {
        packed_server_invalidate_key(KV_MSG_KEY(io->io[i]->msg), io->io[i]->msg->key_len);
        io->io[i]->msg->value_len = 0;
        io->io[i]->msg->type = KV_MSG_OK;
        io->io[i]->msg->q_info = ds_queue.q_info[io->io[i]->worker_id];
        kv_rdma_make_resp(io->io[i]->req_h, (uint8_t *)io->io[i]->msg, KV_MSG_SIZE(io->io[i]->msg));
        kv_mempool_put(io_pool, io->io[i]);
    }
}

static void forward_cb(void *arg) {
    struct io_ctx *io = arg;
    if (io->in_copy_pool) {
        if (io->msg->type == KV_MSG_OK) {
            struct kv_data_store_copy_buf *copy_buf = io->copy_buf;
            kv_mempool_put(copy_pool, io);
            kv_data_store_copy_commit(copy_buf);
        } else if (io->msg->type == KV_MSG_OUTDATED) {
            // retry
            io->msg->type = KV_MSG_SET;
            io->msg->value_len = io->copy_buf->val_len;
            kv_ring_forward(io->fwd_ctx, io->req, io->in_copy_pool, forward_cb, io);
        } else {
            fprintf(stderr, "kv_server: copy forward failed.\n");
            exit(-1);
        }
        return;
    }
    if (io->vnode_type == KV_RING_VNODE && (io->msg_type == KV_MSG_SET || io->msg_type == KV_MSG_DEL)) {
        struct kv_data_store *ds = (workers + io->worker_id)->data_store + io->storage_id;
        kv_data_store_clean(ds, KV_MSG_KEY(io->msg), io->msg->key_len);
    }
    if (io->msg_type == KV_MSG_SET) {
        kv_data_store_set_commit(io->ds_ctx, io->msg->type == KV_MSG_OK);
    } else if (io->msg_type == KV_MSG_BUFFERED_SET) {
        kv_data_store_set_buffered_commit(io->ds_ctx, io->msg->type == KV_MSG_OK);
    } else if (io->msg_type == KV_MSG_DEL) {
        kv_data_store_del_commit(io->ds_ctx, io->msg->type == KV_MSG_OK);
    }
    kv_app_send(io->server_thread, io->msg_type == KV_MSG_BUFFERED_SET ? buffered_send_response : send_response, arg);
}

static void io_fini(bool success, void *arg) {
    struct io_ctx *io = arg;
    if (io->in_copy_pool && success == false) {
        fprintf(stderr, "kv_server: copy failed.\n");
        exit(-1);
    }
    struct kv_data_store *ds = (workers + io->worker_id)->data_store + io->storage_id;
    if (!success) {
        io->msg->type = KV_MSG_ERR;
        kv_ring_forward(io->fwd_ctx, NULL, false, forward_cb, io);
        return;
    }
    if (io->msg_type == KV_MSG_SET || io->msg_type == KV_MSG_DEL) {
        if (io->vnode_type == KV_RING_TAIL && io->has_next_node) {
            io->need_forward = kv_data_store_copy_forward(ds, KV_MSG_KEY(io->msg));
        } else if (io->vnode_type == KV_RING_VNODE)
            io->need_forward = true;
        if (opt.ours) {
            packed_server_invalidate_key(KV_MSG_KEY(io->msg), io->msg->key_len);
        }
    }

    if (io->need_forward == false) {  // is the last node
        if (io->msg->type == KV_MSG_SET) io->msg->value_len = 0;
        io->msg->type = KV_MSG_OK;
    }
    kv_ring_forward(io->fwd_ctx, io->need_forward ? io->req : NULL, io->in_copy_pool, forward_cb, io);
}

static void io_start(void *arg) {
    struct io_ctx *io = arg;
    struct worker_t *self = workers + io->worker_id;
    struct set_buffer *set_buffer = self->set_buffer + io->storage_id;
    io->need_forward = false;
    switch (io->msg->type) {
        case KV_MSG_DEL:
        case KV_MSG_SET:
            if (io->vnode_type == KV_RING_VNODE) kv_data_store_dirty(&self->data_store[io->storage_id], KV_MSG_KEY(io->msg), io->msg->key_len);
            if (io->msg->type == KV_MSG_SET)
                io->ds_ctx = kv_data_store_set(&self->data_store[io->storage_id], KV_MSG_KEY(io->msg), io->msg->key_len,
                                               KV_MSG_VALUE(io->msg), io->msg->value_len, io_fini, arg);
            else {
                assert(io->msg->value_len == 0);
                io->ds_ctx = kv_data_store_delete(&self->data_store[io->storage_id], KV_MSG_KEY(io->msg), io->msg->key_len, io_fini, arg);
            }
            break;
        case KV_MSG_BUFFERED_SET:
            assert(set_buffer->buffer_size < opt.set_batch);
            set_buffer->key[set_buffer->buffer_size] = KV_MSG_KEY(io->msg);
            set_buffer->key_length[set_buffer->buffer_size] = io->msg->key_len;
            set_buffer->value[set_buffer->buffer_size] = KV_MSG_VALUE(io->msg);
            set_buffer->value_length[set_buffer->buffer_size] = io->msg->value_len;
            set_buffer->io[set_buffer->buffer_size] = io;
            set_buffer->buffer_size++;
            gettimeofday(&set_buffer->active, NULL);
            if (set_buffer->buffer_size == opt.set_batch) {
                for (uint32_t i = 0; i < opt.set_batch; ++i) {
                    io->key[i] = set_buffer->key[i];
                    io->key_length[i] = set_buffer->key_length[i];
                    io->value[i] = set_buffer->value[i];
                    io->value_length[i] = set_buffer->value_length[i];
                    io->io[i] = set_buffer->io[i];
                    io->buffer_size = set_buffer->buffer_size;
                }
                io->ds_ctx = kv_data_store_buffered_set(&self->data_store[io->storage_id], io->key, io->key_length, io->value,
                                                        io->value_length, io->value_offset, io->bucket_id, io->seg,
                                                        set_buffer->buffer_size, io_fini, arg);
                set_buffer->buffer_size = 0;
            }
            break;
        case KV_MSG_META_GET:
            *(struct kv_bucket_meta*)KV_MSG_VALUE(io->msg) = kv_bucket_meta_get(&self->data_store[io->storage_id].bucket_log, *(uint64_t *)KV_MSG_KEY(io->msg) >> (64 - self->data_store[io->storage_id].log_bucket_num));
            io->msg->value_len = sizeof(struct kv_bucket_meta);
            assert(io->msg->value_len == PACKED_VALUE_LEN);
            if (opt.ours) {
                int ret = packed_server_put_key(KV_MSG_KEY(io->msg), io->msg->key_len);
                io->msg->put_key_ok = ret >= 0;
                if (io->msg->put_key_ok) {
                    io->msg->slot_id = ret;
                }
                io->msg->ds_id = io->worker_id + io->storage_id * MAX_SSD_WORKERS;
            }
            io_fini(true, arg);
            break;
        case KV_MSG_GET:
            if (kv_data_store_is_dirty(&self->data_store[io->storage_id], KV_MSG_KEY(io->msg), io->msg->key_len) && io->vnode_type == KV_RING_VNODE) {
                io->need_forward = true;
                io_fini(true, arg);
            } else {
                if (opt.ours) {
                    int ret = packed_server_put_key(KV_MSG_KEY(io->msg), io->msg->key_len);
                    io->msg->put_key_ok = ret >= 0;
                    if (io->msg->put_key_ok) {
                        io->msg->slot_id = ret;
                    }
                }
                kv_data_store_get(&self->data_store[io->storage_id], KV_MSG_KEY(io->msg), io->msg->key_len, KV_MSG_VALUE(io->msg),
                                  &io->msg->value_len, NULL, io_fini, arg);
            }
            break;
        case KV_MSG_TEST:
            io_fini(true, io);
            break;
        default:
            assert(false);
    }
}
struct server_copy_ctx {
    struct kv_ring_copy_info *info;
    bool is_start;
    uint32_t ds_id;
    uint64_t key_start;
    uint64_t key_end;
    bool del;
};
static void copy_fini(void *arg) {
    struct server_copy_ctx *ctx = arg;
    kv_ring_stop_copy(ctx->info);
    kv_free(ctx);
}
static void on_copy_fini(bool success, void *arg) {
    assert(success);
    kv_app_send(opt.worker_num, copy_fini, arg);
}
static void on_copy_msg(void *arg) {
    struct server_copy_ctx *ctx = arg;
    struct worker_t *self = workers + ctx->ds_id % MAX_SSD_WORKERS;
    if (ctx->is_start) {
        kv_data_store_copy_add_key_range(&self->data_store[ctx->ds_id / MAX_SSD_WORKERS], (uint8_t *)&ctx->key_start, (uint8_t *)&ctx->key_end, on_copy_fini, ctx);
    } else {
        kv_data_store_copy_del_key_range(&self->data_store[ctx->ds_id / MAX_SSD_WORKERS], (uint8_t *)&ctx->key_start, (uint8_t *)&ctx->key_end, ctx->del);
        kv_free(ctx);
    }
}

static void ring_copy_cb(bool is_start, struct kv_ring_copy_info *info, void *arg) {
    struct server_copy_ctx *ctx = kv_malloc(sizeof(*ctx));
    *ctx = (struct server_copy_ctx){info, is_start, info->ds_id, info->start, info->end, info->del};
    kv_app_send(info->ds_id % MAX_SSD_WORKERS, on_copy_msg, ctx);
}

static void handler(void *req_h, kv_rdma_mr req, void *fwd_ctx, bool has_next_node, uint32_t ds_id, uint32_t vnode_type, void *arg) {
    uint32_t thread_id = kv_app_get_thread_index();
    struct io_ctx *io = kv_mempool_get(io_pool);
    assert(io);
    io->req_h = req_h;
    io->msg = (struct kv_msg *)kv_rdma_get_req_buf(req);
    io->worker_id = ds_id % MAX_SSD_WORKERS;
    io->storage_id = ds_id / MAX_SSD_WORKERS;
    io->server_thread = thread_id;
    io->req = req;
    io->fwd_ctx = fwd_ctx;
    io->has_next_node = has_next_node;
    io->in_copy_pool = false;
    io->vnode_type = vnode_type;
    io->msg_type = io->msg->type;
    kv_app_send(io->worker_id, io_start, io);
}

static void buffer_poller_impl(void *arg) {
    struct worker_t *self = arg;
    for (uint32_t i = 0; i < MAX_STORAGE_STRIDE; ++i) {
        struct set_buffer *set_buffer = self->set_buffer + i;
        struct timeval now;
        gettimeofday(&now, NULL);
        double duration = timeval_diff(&set_buffer->active, &now);
        if (duration > 1.0 / 1000.0 && set_buffer->buffer_size > 0) {
            struct io_ctx *io = set_buffer->io[set_buffer->buffer_size - 1];
            struct worker_t *self = workers + io->worker_id;
            assert(self == arg);
            for (uint32_t i = 0; i < set_buffer->buffer_size; ++i) {
                io->key[i] = set_buffer->key[i];
                io->key_length[i] = set_buffer->key_length[i];
                io->value[i] = set_buffer->value[i];
                io->value_length[i] = set_buffer->value_length[i];
                io->io[i] = set_buffer->io[i];
                io->buffer_size = set_buffer->buffer_size;
            }
            io->ds_ctx = kv_data_store_buffered_set(&self->data_store[i], io->key, io->key_length, io->value,
                                                    io->value_length, io->value_offset, io->bucket_id, io->seg,
                                                    set_buffer->buffer_size, io_fini, io);
            set_buffer->buffer_size = 0;
        }
    }
}

static int buffer_poller(void *arg) {
    for (size_t i = 0; i < opt.worker_num; i++) {
        kv_app_send(i, buffer_poller_impl, workers + i);
    }
    return 0;
}

static void ring_init_cb(void *arg) {
    copy_pool = kv_mempool_create(opt.copy_concurrency * 2, sizeof(struct io_ctx));
    server_mrs = kv_rdma_alloc_bulk(server, KV_RDMA_MR_SERVER, workers[0].storage[0].block_size + sizeof(struct kv_msg) + KV_MAX_KEY_LENGTH, opt.copy_concurrency * 2);
    for (size_t i = 0; i < opt.copy_concurrency * 2; i++) {
        struct io_ctx *io = kv_mempool_get(copy_pool);
        io->req = kv_rdma_mrs_get(server_mrs, i);
        kv_mempool_put(copy_pool, io);
    }
}
static uint32_t io_cnt;
uint64_t log_bucket_num = 48;
static void ring_init(void *arg) {
    if (--io_cnt) return;
    buf_poller = kv_app_poller_register(buffer_poller, NULL, 1000);
    server = kv_ring_init(opt.etcd_ip, opt.etcd_port, opt.thread_num, NULL, NULL);
    io_pool = kv_mempool_create(opt.concurrent_io_num, sizeof(struct io_ctx));
    kv_ring_server_init(opt.local_ip, opt.local_port, opt.ring_num, opt.vid_per_ssd, opt.ssd_num, opt.rpl_num,
                        log_bucket_num, opt.concurrent_io_num, sizeof(struct kv_msg) + KV_MAX_KEY_LENGTH + workers[0].storage[0].block_size,
                        handler, NULL, ring_init_cb, NULL);
    kv_ring_register_copy_cb(ring_copy_cb, NULL);
}

static void copy_get_buf(uint8_t *key, uint8_t key_len, struct kv_data_store_copy_buf *buf, void *cb_arg) {
    struct io_ctx *io = kv_mempool_get(copy_pool);
    io->req_h = NULL;
    io->msg = (struct kv_msg *)kv_rdma_get_req_buf(io->req);
    io->worker_id = kv_app_get_thread_index();
    io->server_thread = opt.worker_num + random() % opt.thread_num;
    io->msg->type = KV_MSG_SET;
    io->fwd_ctx = NULL;
    io->has_next_node = false;
    io->need_forward = true;
    io->in_copy_pool = true;
    io->msg->key_len = key_len;
    kv_memcpy(KV_MSG_KEY(io->msg), key, key_len);
    io->msg->value_len = buf->val_len;
    buf->val_buf = KV_MSG_VALUE(io->msg);
    buf->ctx = io;
    io->copy_buf = buf;
}

static void signal_handler(int signal_number) {
    if (buf_poller) kv_app_poller_unregister(&buf_poller);
    kv_ring_fini(ring_fini_cb, NULL);
}

static void worker_init(void *arg) {
    struct worker_t *self = arg;
    if (self == workers) {
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
    }
    for (uint32_t i = 0; i < MAX_STORAGE_STRIDE; ++i) {
        if (WORKER_INDEX >= opt.ssd_num) {
            break;
        }
        kv_storage_init(&self->storage[i], WORKER_INDEX);
        uint64_t bucket_num = KV_NUM_ITEMS / KV_ITEM_PER_BUCKET / opt.ssd_num;
        uint64_t value_log_block_num = self->storage[i].num_blocks * 0.95 - 2 * bucket_num;
        kv_data_store_init(&self->data_store[i], &self->storage[i], 0, bucket_num, log_bucket_num, value_log_block_num, 512, &ds_queue, WORKER_INDEX);
        kv_data_store_copy_init(&self->data_store[i], copy_get_buf, NULL, opt.copy_concurrency / opt.ssd_num, io_fini);
    }
    kv_app_send(opt.worker_num, ring_init, NULL);
}
#define KEY_PER_BKT_SEGMENT (KV_ITEM_PER_BUCKET)
// memory usage per key: 5/KEY_PER_BKT_SEGMENT bytes
int main(int argc, char **argv) {
#ifdef NDEBUG
    printf("NDEBUG\n");
#else
    printf("DEBUG (low performance)\n");
#endif
    get_options(argc, argv);
    if (opt.ours) packed_server_init(opt.server_conf_file);
    while ((1ULL << log_bucket_num) >= KV_NUM_ITEMS / KEY_PER_BKT_SEGMENT) log_bucket_num--;
    ++log_bucket_num;
    opt.worker_num = MAX_SSD_WORKERS;
    opt.ring_num = opt.vid_per_ssd * opt.ssd_num;
    struct kv_app_task *task = calloc(opt.worker_num + opt.thread_num, sizeof(struct kv_app_task));
    workers = calloc(opt.worker_num, sizeof(struct worker_t));
    for (size_t i = 0; i < opt.worker_num; i++) {
        task[i].func = worker_init;
        task[i].arg = workers + i;
    }
    for (size_t i = 0; i < opt.thread_num; i++) {
        task[opt.worker_num + i] = (struct kv_app_task){NULL, NULL};
    }
    io_cnt = opt.worker_num;
    kv_ds_queue_init(&ds_queue, opt.ssd_num);
    kv_app_start(opt.json_config_file, opt.worker_num + opt.thread_num, task);
    kv_ds_queue_fini(&ds_queue);
    free(workers);
    free(task);
    if (opt.ours) packed_server_fini();
    return 0;
}