#include <assert.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "../../kv_app.h"
#include "../../kv_data_store.h"
#include "../../kv_msg.h"
#include "../../kv_ring.h"
#include "../../utils/city.h"
#include "../../utils/ditto_wrapper.h"
#include "../../utils/timing.h"
#include "../../ycsb/kv_ycsb.h"

struct {
    uint64_t num_items, operation_cnt;
    uint32_t value_size;
    uint32_t ssd_num;
    uint32_t thread_num;
    uint32_t producer_num;
    uint32_t concurrent_io_num;
    int stat_interval;
    int client_id;
    bool seq_read, fill, seq_write, del;
    char json_config_file[1024];
    char workload_file[1024];
    char client_conf_file[1024];
    char server_ip[32];
    char server_port[16];
    char memcached_ip[32];
    bool ditto;
    bool ours;
    int breakdown_stage;
} opt = {.num_items = 100000000,
         .operation_cnt = 512,
         .ssd_num = 4,
         .thread_num = 2,
         .producer_num = 32,
         .value_size = 1024,
         .concurrent_io_num = 32,
         .stat_interval = -1,
         .client_id = 1,
         .json_config_file = "client.config.json",
         .workload_file = "workloada.spec",
         .client_conf_file = "app/leed/ditto/experiments/configs/client_conf_sample.json",
         .server_ip = "10.1.4.6",
         .server_port = "2379",
         .memcached_ip = "10.1.4.6",
         .ditto = false,
         .ours = false,
         .breakdown_stage = 3,
         .seq_read = false,
         .seq_write = false,
         .del = false,
         .fill = false};
         
static void help(void) {
    printf("Program options:\n");
    printf("  -h               Display this help message\n");
    printf("  -d <ssd_num>     Set the number of SSDs: %u\n", opt.ssd_num);
    printf("  -c <config_file> Set the SPDK JSON config file: %s\n", opt.json_config_file);
    printf("  -w <workload_file> Set the YCSB workload file: %s\n", opt.workload_file);
    printf("  -f <client_conf> Set the Ditto client config file: %s\n", opt.client_conf_file);
    printf("  -P <producer_num> Set the number of YCSB threads : %u\n", opt.producer_num);
    printf("  -i <io_num>      Set the number of concurrent I/Os: %u\n", opt.concurrent_io_num);
    printf("  -s <etcd_ip>     Set the etcd's IP: %s\n", opt.server_ip);
    printf("  -p <etcd_port>   Set the etcd's port: %s\n", opt.server_port);
    printf("  -m <memcached_ip> Set the memcached's IP: %s\n", opt.memcached_ip);
    printf("  -T <thread_num>  Set the number of threads for handling RDMA requests: %u\n", opt.thread_num);
    printf("  -I <stat_interval> Set the statistics interval (set to -1 to disable): %d\n", opt.stat_interval);
    printf("  -x               Start client ID: %d\n", opt.client_id);
    printf("  -R               Perform sequential read operations\n");
    printf("  -W               Perform sequential write operations\n");
    printf("  -D               Perform delete operations\n");
    printf("  -F               Perform fill operations\n");
    printf("  -C <ditto/ours>  Enable caching\n");
    printf("  -B <breakdown_stage> 0: baseline, 1: w/ offloaded read, 2: w/ inline cache, 3: w/ batched write\n");
}

static void get_options(int argc, char **argv) {
    int ch;
    while ((ch = getopt(argc, argv, "htr:v:d:P:c:i:p:s:m:T:w:f:I:x:RWFDC:B:")) != -1) switch (ch) {
            case 'w':
                strcpy(opt.workload_file, optarg);
                break;
            case 'c':
                strcpy(opt.json_config_file, optarg);
                break;
            case 'f':
                strcpy(opt.client_conf_file, optarg);
                break;
            case 'i':
                opt.concurrent_io_num = atol(optarg);
                break;
            case 'd':
                opt.ssd_num = atol(optarg);
                break;
            case 'P':
                opt.producer_num = atol(optarg);
                break;
            case 'T':
                opt.thread_num = atol(optarg);
                break;
            case 's':
                strcpy(opt.server_ip, optarg);
                break;
            case 'p':
                strcpy(opt.server_port, optarg);
                break;
            case 'm':
                strcpy(opt.memcached_ip, optarg);
                break;
            case 'I':
                opt.stat_interval = atol(optarg);
                break;
            case 'x':
                opt.client_id = atol(optarg);
                break;
            case 'B':
                opt.breakdown_stage = atol(optarg);
                if (opt.breakdown_stage < 0 || opt.breakdown_stage > 3) {
                    help();
                    exit(-1);
                }
                break;
            case 'R':
                opt.seq_read = true;
                break;
            case 'W':
                opt.seq_write = true;
                break;
            case 'D':
                opt.del = true;
                break;
            case 'F':
                opt.fill = true;
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

struct io_buffer_t {
    kv_rdma_mr req, resp;
    uint32_t worker_id;
    uint32_t producer_id;
    bool read_modify_write, is_finished, ditto_fill, ditto_clear;
    uint32_t retry_cnt;
    struct timeval io_start;
    kv_data_store_ctx ds_ctx;
} * io_buffers;

struct worker_t {
    struct kv_storage storage;
    struct kv_data_store data_store;
} * workers;

struct kv_ds_queue ds_queue;

struct producer_t {
    uint64_t start_io, end_io;
    uint64_t iocnt;
    double latency_sum;
    uint32_t io_per_record;
    _Atomic uint64_t counter;  // for real time thourghput
} * producers;

static void *tp_poller = NULL;
#define LATENCY_MAX_RECORD 0x100000  // 1M
static double latency_records[LATENCY_MAX_RECORD];

static struct timeval tv_start, tv_end;
enum {
    INIT,
    FILL,
    TRANSACTION,
    SEQ_READ,
    SEQ_WRITE,
    DEL,
} state = INIT;
kv_rdma_handle rdma;
kv_rdma_mrs_handle req_mrs, resp_mrs;
kv_ycsb_handle workloads[64];

static void worker_stop(void *arg) {
    struct worker_t *self = arg;
    kv_data_store_fini(&self->data_store);
    kv_storage_fini(&self->storage);
    kv_app_stop(0);
}
static void thread_stop(void *arg) { kv_app_stop(0); }
static void ring_fini_cb(void *arg) {
    for (size_t i = 0; i < opt.ssd_num; i++) kv_app_send(i, worker_stop, workers + i);
    for (size_t i = 0; i < opt.thread_num + opt.producer_num; i++) kv_app_send(opt.ssd_num + i, thread_stop, NULL);
}
static void stop(void) {
    kv_rdma_free_bulk(req_mrs);
    kv_rdma_free_bulk(resp_mrs);
    if (tp_poller) kv_app_poller_unregister(&tp_poller);
    kv_ring_fini(ring_fini_cb, NULL);
}

static int double_cmp(const void *_a, const void *_b) {
    const double *a = _a, *b = _b;
    if (*a == *b) return 0;
    return *a < *b ? -1 : 1;
}
static void test(void *arg);
static void io_start(void *arg);
static void meta_get_cb(void *arg);
static void test_fini(void *arg) {  // always running on producer 0
    static uint64_t total_io = 0;
    static uint32_t io_per_record = 0;
    static uint32_t producer_cnt = 1;
    if (--producer_cnt) return;
    gettimeofday(&tv_end, NULL);
    producer_cnt = opt.producer_num;
    double latency_sum = 0;
    for (size_t i = 0; i < opt.producer_num; i++) {
        latency_sum += producers[i].latency_sum;
        producers[i].latency_sum = 0;
    }
    if (total_io) {
        qsort(latency_records, total_io / io_per_record, sizeof(double), double_cmp);
        printf("99.9%%  tail latency: %lf us\n", latency_records[(uint32_t)(total_io * 0.999 / io_per_record)] * 1000000);
        printf("average latency: %lf us\n", latency_sum * 1000000 / total_io);
    }
    switch (state) {
        case INIT:
            assert(opt.value_size <= workers[0].storage.block_size);
            req_mrs = kv_rdma_alloc_bulk(rdma, KV_RDMA_MR_REQ, workers[0].storage.block_size + sizeof(struct kv_msg) + KV_MAX_KEY_LENGTH, opt.concurrent_io_num);
            resp_mrs =
                kv_rdma_alloc_bulk(rdma, KV_RDMA_MR_RESP, workers[0].storage.block_size + sizeof(struct kv_msg) + KV_MAX_KEY_LENGTH, opt.concurrent_io_num);
            for (size_t i = 0; i < opt.concurrent_io_num; i++) {
                io_buffers[i].req = kv_rdma_mrs_get(req_mrs, i);
                io_buffers[i].resp = kv_rdma_mrs_get(resp_mrs, i);
            }
            printf("rdma client initialized in %lf s.\n", timeval_diff(&tv_start, &tv_end));
            if (opt.fill) {
                total_io = opt.num_items;
                state = FILL;
            } else {
                state = opt.del ? DEL : opt.seq_read ? SEQ_READ
                                    : opt.seq_write  ? SEQ_WRITE
                                                     : TRANSACTION;
                total_io = opt.operation_cnt;
            }
            break;
        case FILL:
            printf("Write rate: %lf\n", ((double)opt.num_items / timeval_diff(&tv_start, &tv_end)));
            puts("db created successfully.");
            state = opt.del ? DEL : opt.seq_read ? SEQ_READ
                                : opt.seq_write  ? SEQ_WRITE
                                                 : TRANSACTION;
            total_io = opt.operation_cnt;
            break;
        case SEQ_WRITE:
            printf("SEQ_WRITE rate: %lf\n", ((double)opt.operation_cnt / timeval_diff(&tv_start, &tv_end)));
            stop();
            return;
        case SEQ_READ:
            printf("SEQ_READ rate: %lf\n", ((double)opt.operation_cnt / timeval_diff(&tv_start, &tv_end)));
            stop();
            return;
        case DEL:
            printf("DEL rate: %lf\n", ((double)opt.operation_cnt / timeval_diff(&tv_start, &tv_end)));
            stop();
            return;
        case TRANSACTION:
            printf("TRANSACTION rate: %lf\n", ((double)opt.operation_cnt / timeval_diff(&tv_start, &tv_end)));
            stop();
            return;
    }
    for (size_t i = 0; i < opt.concurrent_io_num; i++) io_buffers[i].is_finished = false;
    io_per_record = (uint32_t)ceil(((double)total_io) / LATENCY_MAX_RECORD);
    uint64_t io_per_producer = total_io / opt.producer_num;
    for (size_t i = 0; i < opt.producer_num; i++) {
        producers[i].io_per_record = io_per_record;
        producers[i].iocnt = opt.concurrent_io_num / opt.producer_num;
        producers[i].start_io = i * io_per_producer;
        producers[i].end_io = (i + 1) * io_per_producer;
        for (size_t j = i * producers[i].iocnt; j < (i + 1) * producers[i].iocnt; j++) {
            io_buffers[j].producer_id = i;
            kv_app_send(opt.ssd_num + opt.thread_num + i, test, io_buffers + j);
        }
    }
    producers[opt.producer_num - 1].end_io = total_io;
    gettimeofday(&tv_start, NULL);
}

static inline void do_transaction(struct io_buffer_t *io, struct kv_msg *msg) {
    msg->key_len = 16;
    enum kv_ycsb_operation op = kv_ycsb_next(workloads[io->producer_id], false, KV_MSG_KEY(msg), KV_MSG_VALUE(msg));
    switch (op) {
        case YCSB_READMODIFYWRITE:
            io->read_modify_write = true;
        // fall through
        case YCSB_READ:
            msg->type = KV_MSG_GET;
            msg->value_len = 0;
            break;
        case YCSB_UPDATE:
        case YCSB_INSERT:
            msg->type = KV_MSG_SET;
            msg->value_len = opt.value_size;
            break;
        default:
            assert(false);
    }
}

static void test(void *arg) {
    struct io_buffer_t *io = arg;
    struct producer_t *p = io ? producers + io->producer_id : producers;
    if (io && io->is_finished) {
        io->retry_cnt = 0;
        p->counter++;
        if (opt.ditto) {
            struct kv_msg *msg = (struct kv_msg *)kv_rdma_get_resp_buf(io->resp);
            if (io->ditto_fill || io->ditto_clear) {
                ditto_set(io->producer_id, KV_MSG_KEY(msg), msg->key_len, KV_MSG_VALUE(msg), msg->value_len);
            }
        } else if (opt.ours && opt.breakdown_stage >= 2) {
            struct kv_msg *msg = (struct kv_msg *)kv_rdma_get_resp_buf(io->resp);
            if (io->ditto_fill) {
                if (msg->put_key_ok) {
                    packed_set(io->producer_id, KV_MSG_KEY(msg), msg->key_len, KV_MSG_VALUE(msg), msg->value_len,
                               msg->slot_id);
                } else {
                    packed_set_dummy(io->producer_id);
                }
            }
        }
        struct timeval io_end;
        gettimeofday(&io_end, NULL);
        double latency = timeval_diff(&io->io_start, &io_end);
        p->latency_sum += latency;
        if (p->start_io % p->io_per_record == 0) {
            latency_records[p->start_io / p->io_per_record] = latency;
        }
        if (((struct kv_msg *)kv_rdma_get_resp_buf(io->resp))->type != KV_MSG_OK) {
            fprintf(stderr, "io fail. \n");
            exit(-1);
        } else if (io->read_modify_write) {
            struct kv_msg *msg = (struct kv_msg *)kv_rdma_get_req_buf(io->req);
            msg->type = opt.ours && opt.breakdown_stage == 3 ? KV_MSG_BUFFERED_SET : KV_MSG_SET;
            msg->value_len = opt.value_size;
            io->read_modify_write = false;
            io->ditto_fill = false;
            io->ditto_clear = true;
            kv_ring_dispatch(io->req, io->resp, kv_rdma_get_resp_buf(io->resp), test, io);
            return;
        }
    }

    if (p->start_io == p->end_io) {
        if (--p->iocnt == 0) {
            kv_app_send(opt.ssd_num + opt.thread_num, test_fini, NULL);
        }
        return;
    }
    struct kv_msg *msg = (struct kv_msg *) kv_rdma_get_req_buf(io->req);
    if (io->retry_cnt == 0) {
        io->read_modify_write = false;
        switch (state) {
            case SEQ_WRITE:
            case FILL:
                msg->type = KV_MSG_SET;
                msg->key_len = 16;
                msg->value_len = opt.value_size;
                kv_ycsb_next(workloads[0], true, KV_MSG_KEY(msg), KV_MSG_VALUE(msg));
                break;
            case SEQ_READ:
                msg->type = KV_MSG_GET;
                msg->key_len = 16;
                msg->value_len = 0;
                kv_ycsb_next(workloads[0], true, KV_MSG_KEY(msg), NULL);
                break;
            case DEL:
                msg->type = KV_MSG_DEL;
                msg->key_len = 16;
                msg->value_len = 0;
                kv_ycsb_next(workloads[0], true, KV_MSG_KEY(msg), NULL);
                break;
            case TRANSACTION:
                do_transaction(io, msg);
                break;
            case INIT:
                assert(false);
        }
        io->is_finished = true;
        p->start_io++;
        gettimeofday(&io->io_start, NULL);
        io->worker_id = (*(uint64_t * )KV_MSG_KEY(msg) >> (64 - 48)) % opt.ssd_num;
    }
    if (msg->type == KV_MSG_GET) {
        int ret;
        if (opt.ditto) {
            ret = ditto_get(io->producer_id, KV_MSG_KEY(msg), msg->key_len, KV_MSG_VALUE(msg), &msg->value_len);
        } else if (opt.ours && opt.breakdown_stage >= 1) {
            msg->type = KV_MSG_META_GET;
            ret = opt.breakdown_stage >= 2 ? packed_get(io->producer_id, KV_MSG_KEY(msg), msg->key_len, KV_MSG_VALUE(msg), &msg->value_len) : -1;
            if (opt.breakdown_stage >= 2 && io->retry_cnt == 1) {
                packed_get_retry(io->producer_id);
            }
        } else {
            ret = -1;
        }
        if (ret == -2) {
            // retry
            io->is_finished = false;
            if (++io->retry_cnt < 100) {
                msg->type = KV_MSG_GET;
                kv_app_send(opt.ssd_num + opt.thread_num + io->producer_id, test, arg);
                return;
            }
        }
        io->is_finished = true;
        if (ret != 0 || msg->value_len == 0) {
            io->ditto_fill = true;
            io->ditto_clear = false;
            kv_ring_dispatch(io->req, io->resp, kv_rdma_get_resp_buf(io->resp), msg->type == KV_MSG_META_GET ? meta_get_cb : test, io);
        } else {
            io->ditto_fill = false;
            io->ditto_clear = false;
            test(arg);
        }
    } else {
        io->ditto_fill = false;
        io->ditto_clear = true;
        if (opt.ours && opt.breakdown_stage == 3 && msg->type == KV_MSG_SET) {
            msg->type = KV_MSG_BUFFERED_SET;
        }
        kv_ring_dispatch(io->req, io->resp, kv_rdma_get_resp_buf(io->resp), test, io);
    }
}

static void io_fini(bool success, void *arg) {
    struct io_buffer_t *io = arg;
    struct kv_msg *msg = (struct kv_msg *)kv_rdma_get_resp_buf(io->resp);
    assert(msg->value_len <= PACKED_VALUE_LEN);
    if (!success) {
        msg->type = KV_MSG_ERR;
    }
    if (msg->type == KV_MSG_SET) {
        kv_data_store_set_commit(io->ds_ctx, success);
    } else if (msg->type == KV_MSG_BUFFERED_SET) {
        kv_data_store_set_buffered_commit(io->ds_ctx, success);
    } else if (msg->type == KV_MSG_DEL) {
        kv_data_store_del_commit(io->ds_ctx, success);
    } else if (msg->type == KV_MSG_META_GET) {
        msg->type = KV_MSG_GET;
    }
    kv_app_send(opt.ssd_num + opt.thread_num + io->producer_id, test, arg);
}

static void io_start(void *arg) {
    struct io_buffer_t *io = arg;
    struct worker_t *self = workers + io->worker_id;
    struct kv_msg *msg = (struct kv_msg *)kv_rdma_get_resp_buf(io->resp);
    assert(msg->type == KV_MSG_GET);
    kv_data_store_get(&self->data_store, KV_MSG_KEY(msg), msg->key_len, KV_MSG_VALUE(msg), &msg->value_len, (struct kv_bucket_meta *)KV_MSG_VALUE(msg), io_fini, arg);
}

static void meta_get_cb(void *arg) {
    struct io_buffer_t *io = arg;
    struct kv_msg *msg = (struct kv_msg *)kv_rdma_get_resp_buf(io->resp);
    io->worker_id = msg->ds_id;
    if (!msg->put_key_ok) {
        if (++io->retry_cnt < 100) {
            ((struct kv_msg *) kv_rdma_get_req_buf(io->req))->type = KV_MSG_GET;
            kv_app_send(opt.ssd_num + opt.thread_num + io->producer_id, test, arg);
            return;
        }
    }
    kv_app_send(io->worker_id, io_start, io);
}

static struct timeval stat_tv;
static int throughput_poller(void *arg) {
    struct timeval now;
    gettimeofday(&now, NULL);
    double duration = timeval_diff(&stat_tv, &now);
    stat_tv = now;
    uint64_t sum = 0;
    for (size_t i = 0; i < opt.producer_num; i++) {
        sum += atomic_exchange(&producers[i].counter, 0);
    }
    printf("throughput: %lf IOPS\n", (double)sum / duration);
    return 0;
}

static void signal_handler(int signal_number) {
    stop();
}

static void ring_ready_cb(void *arg) { kv_app_send(opt.ssd_num + opt.thread_num, test, NULL); }
uint64_t log_bucket_num = 48;
static void ring_init(void *arg) {
    gettimeofday(&stat_tv, NULL);
    if (opt.stat_interval > 0)
        tp_poller = kv_app_poller_register(throughput_poller, NULL, opt.stat_interval * 1000);
    rdma = kv_ring_init(opt.server_ip, opt.server_port, opt.thread_num, ring_ready_cb, NULL);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}

static void worker_init(void *arg) {
    struct worker_t *self = arg;
    kv_storage_init(&self->storage, self - workers);
    uint64_t bucket_num = KV_NUM_ITEMS / KV_ITEM_PER_BUCKET / opt.ssd_num;
    uint64_t value_log_block_num = self->storage.num_blocks * 0.95 - 2 * bucket_num;
    kv_data_store_init(&self->data_store, &self->storage, 0, bucket_num, log_bucket_num, value_log_block_num, 512, &ds_queue, self - workers);
    kv_app_send(opt.ssd_num + opt.thread_num, test, NULL);
}

static void* ycsb_loader(void* arg) {
    kv_ycsb_handle *workload = arg;
    kv_ycsb_init(workload, opt.workload_file, &opt.num_items, &opt.operation_cnt, &opt.value_size);
    return NULL;
}

int main(int argc, char **argv) {
#ifdef NDEBUG
    printf("NDEBUG\n");
#else
    printf("DEBUG (low performance)\n");
#endif
    get_options(argc, argv);
    if (opt.ditto) ditto_init(opt.producer_num, opt.client_id, opt.client_conf_file, opt.memcached_ip);
    if (opt.ours) packed_init(opt.producer_num, opt.client_id, opt.client_conf_file, opt.memcached_ip);
    io_buffers = calloc(opt.concurrent_io_num, sizeof(struct io_buffer_t));
    struct kv_app_task *task = calloc(opt.ssd_num + opt.thread_num + opt.producer_num, sizeof(struct kv_app_task));
    workers = calloc(opt.ssd_num, sizeof(struct worker_t));
    for (size_t i = 0; i < opt.ssd_num; i++) {
        task[i].func = worker_init;
        task[i].arg = workers + i;
    }
    for (size_t i = opt.ssd_num; i < opt.ssd_num + opt.thread_num + opt.producer_num; i++) {
        task[i] = (struct kv_app_task){NULL, NULL};
    }
    task[opt.ssd_num].func = ring_init;
    producers = calloc(opt.producer_num, sizeof(struct producer_t));
    *producers = (struct producer_t){0, 0, opt.ssd_num + 1};
    kv_ds_queue_init(&ds_queue, opt.ssd_num);
    pthread_t ycsb_loader_threads[opt.producer_num];
    for (size_t i = 0; i < opt.producer_num; ++i) {
        pthread_create(ycsb_loader_threads + i, NULL, ycsb_loader, (void *)(workloads + i));
    }
    for (size_t i = 0; i < opt.producer_num; ++i) {
        pthread_join(ycsb_loader_threads[i], NULL);
    }
    gettimeofday(&tv_start, NULL);
    kv_app_start(opt.json_config_file, opt.ssd_num + opt.thread_num + opt.producer_num, task);
    for (size_t i = 0; i < opt.producer_num; ++i) {
        kv_ycsb_fini(workloads[i]);
    }
    kv_ds_queue_fini(&ds_queue);
    free(io_buffers);
    free(producers);
    if (opt.ditto) ditto_fini(opt.producer_num);
    if (opt.ours) packed_fini(opt.producer_num);
    free(task);
}