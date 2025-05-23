#include "kv_data_store.h"

#include <assert.h>
#include <stdio.h>
#include <sys/queue.h>

#include "kv_app.h"
#include "kv_memory.h"

// --- queue ---
struct queue_entry {
    struct kv_data_store *self;
    enum kv_ds_op op;
    kv_task_cb fn;
    void *ctx;
    kv_data_store_cb cb;
    void *cb_arg;
    STAILQ_ENTRY(queue_entry)
    entry;
};
STAILQ_HEAD(queue_head, queue_entry);

static void *enqueue(struct kv_data_store *self, enum kv_ds_op op, kv_task_cb fn, void *ctx, kv_data_store_cb cb,
                     void *cb_arg) {
    struct queue_entry *entry = kv_malloc(sizeof(struct queue_entry));
    *entry = (struct queue_entry){self, op, fn, ctx, cb, cb_arg};
    struct kv_ds_q_info q_info = self->ds_queue->q_info[self->ds_id];
    if (kv_ds_queue_find(&q_info, NULL, 1, kv_ds_op_cost(op))) {
        self->ds_queue->q_info[self->ds_id] = q_info;
        kv_app_send(self->bucket_log.log.thread_index, fn, ctx);
    } else {
        STAILQ_INSERT_TAIL((struct queue_head *)self->q, entry, entry);
    }
    return entry;
}
static void dequeue(bool success, void *arg) {
    struct queue_entry *entry = arg;
    struct kv_data_store *self = entry->self;
    struct kv_ds_q_info q_info = self->ds_queue->q_info[self->ds_id];
    q_info.size -= kv_ds_op_cost(entry->op);
    while (!STAILQ_EMPTY((struct queue_head *)self->q)) {
        struct queue_entry *first = STAILQ_FIRST((struct queue_head *)self->q);
        if (kv_ds_queue_find(&q_info, NULL, 1, kv_ds_op_cost(first->op))) {
            kv_app_send(self->bucket_log.log.thread_index, first->fn, first->ctx);
            STAILQ_REMOVE_HEAD((struct queue_head *)self->q, entry);
        } else
            break;
    }
    self->ds_queue->q_info[self->ds_id] = q_info;
    if (entry->cb) entry->cb(success, entry->cb_arg);
    kv_free(entry);
}

// --- init & fini ---
static inline uint64_t kv_data_store_bucket_id(struct kv_data_store *self, uint8_t *key) {
    return *(uint64_t *)key >> (64 - self->log_bucket_num);
}

static inline bool compare_keys(const uint8_t *key1, size_t key1_len, const uint8_t *key2, size_t key2_len) {
    return key1_len == key2_len && !kv_memcmp8(key1, key2, key1_len);
}

void kv_data_store_init(struct kv_data_store *self, struct kv_storage *storage, uint64_t base, uint64_t num_buckets, uint64_t log_bucket_num,
                        uint64_t value_log_block_num, uint32_t compact_buf_len, struct kv_ds_queue *ds_queue, uint32_t ds_id) {
    self->log_bucket_num = log_bucket_num;
    kv_bucket_log_init(&self->bucket_log, storage, base, num_buckets);
    kv_value_log_init(&self->value_log, storage, &self->bucket_log, base + self->bucket_log.log.size,
                      value_log_block_num, compact_buf_len);
    uint64_t value_log_size = self->value_log.log.size + self->value_log.id_log_size;
    if (self->bucket_log.log.size + value_log_size > storage->num_blocks) {
        fprintf(stderr, "kv_data_store_init: Not enough space.\n");
        exit(-1);
    }
    printf("bucket log size: %lf GB\n", ((double)self->bucket_log.log.size) * storage->block_size / (1 << 30));
    printf("value log size: %lf GB\n", ((double)value_log_size) * storage->block_size / (1 << 30));
    self->ds_queue = ds_queue;
    self->ds_id = ds_id;
    self->ds_queue->q_info[self->ds_id] = (struct kv_ds_q_info){.cap = 1024, .size = 0};
    self->dirty_set = kv_bucket_key_set_init();
    self->q = kv_malloc(sizeof(struct queue_head));
    STAILQ_INIT((struct queue_head *)self->q);
}

void kv_data_store_fini(struct kv_data_store *self) {
    kv_bucket_log_fini(&self->bucket_log);
    kv_value_log_fini(&self->value_log);
    kv_bucket_key_set_fini(self->dirty_set);
    kv_free(self->q);
}

// --- debug ---
static void print_buckets(struct kv_bucket *buckets) __attribute__((unused));
static void print_buckets(struct kv_bucket *buckets) {
    printf("bucket id %lu: \n", (uint64_t)buckets->id);
    for (struct kv_bucket *bucket = buckets; bucket - buckets < buckets->chain_length; ++bucket) {
        printf("chain index %u: ", bucket->chain_index);
        for (struct kv_item *item = bucket->items; item - bucket->items < KV_ITEM_PER_BUCKET; ++item)
            if (KV_EMPTY_ITEM(item))
                printf("0 ");
            else
                printf("1 ");
        puts("");
    }
    puts("-----");
}

//--- find item ---

static void find_item_plus(struct kv_data_store *self, struct kv_bucket_segment *seg, uint8_t *key, uint8_t key_length,
                           struct kv_item **located_item) {
    assert(located_item);
    struct kv_bucket_chain_entry *ce;
    TAILQ_FOREACH(ce, &seg->chain, entry) {
        for (struct kv_bucket *bucket = ce->bucket; bucket - ce->bucket < ce->len; ++bucket)
            for (struct kv_item *item = bucket->items; item - bucket->items < KV_ITEM_PER_BUCKET; ++item) {
                // if(KV_EMPTY_ITEM(item)) continue; // empty item key_length != ctx->key_length
                if (compare_keys(item->key, item->key_length, key, key_length)) {
                    // job done!
                    *located_item = item;
                    return;
                }
            }
    }
    *located_item = NULL;
}

// --- find empty ---
static struct kv_item *find_empty(struct kv_data_store *self, struct kv_bucket_segment *seg) {
    struct kv_bucket_chain_entry *ce;
    TAILQ_FOREACH(ce, &seg->chain, entry) {
        for (struct kv_bucket *bucket = ce->bucket; bucket - ce->bucket < ce->len; ++bucket)
            for (struct kv_item *item = bucket->items; item - bucket->items < KV_ITEM_PER_BUCKET; ++item)
                if (KV_EMPTY_ITEM(item)) return item;
    }
    if (kv_bucket_alloc_extra(&self->bucket_log, seg)) {
        ce = TAILQ_LAST(&seg->chain, kv_bucket_chain);
        return ce->bucket[ce->len - 1].items;
    }
    return NULL;
}

static void fill_the_hole(struct kv_data_store *self, struct kv_bucket_segment *seg) {
    if (TAILQ_FIRST(&seg->chain)->bucket->chain_length == 1) return;

    struct kv_bucket_chain_entry *ce = TAILQ_LAST(&seg->chain, kv_bucket_chain);
    struct kv_bucket *last_bucket = &ce->bucket[ce->len - 1];
    struct kv_item *item_to_move = last_bucket->items;
    TAILQ_FOREACH(ce, &seg->chain, entry) {
        for (struct kv_bucket *bucket = ce->bucket; bucket - ce->bucket < ce->len && bucket != last_bucket; ++bucket)
            for (struct kv_item *item = bucket->items; item - bucket->items < KV_ITEM_PER_BUCKET; ++item)
                if (KV_EMPTY_ITEM(item)) {
                    while (KV_EMPTY_ITEM(item_to_move)) {
                        if (++item_to_move - last_bucket->items == KV_ITEM_PER_BUCKET) {
                            kv_bucket_free_extra(seg);
                            return;
                        }
                    }
                    *item = *item_to_move;
                    item_to_move->key_length = 0;
                };
    }
}

// --- set ---
struct buffered_set_ctx {
    uint32_t buffer_size;
    uint8_t **key;
    uint8_t *key_length;
    uint8_t value[KV_BLK_SIZE];
    uint32_t *value_length;
    uint64_t *value_offset;
    uint64_t *bucket_id;
    struct kv_bucket_segment *seg;
};

struct set_ctx {
    struct kv_data_store *self;
    uint8_t *key;
    uint8_t key_length;
    uint8_t *value;
    uint32_t value_length;
    kv_data_store_cb cb;
    void *cb_arg;
    uint64_t bucket_id;
    uint32_t io_cnt;
    struct kv_bucket_segments segs;
    struct kv_bucket_segment seg;
    uint64_t value_offset;
    bool success;
    struct buffered_set_ctx set_ctx_buffer;
};

void kv_data_store_set_commit(kv_data_store_ctx arg, bool success) {
    struct set_ctx *ctx = arg;
    if (success) kv_bucket_seg_commit(&ctx->self->bucket_log, &ctx->seg);
    kv_bucket_unlock(&ctx->self->bucket_log, &ctx->segs);
    kv_free(ctx);
}

void kv_data_store_set_buffered_commit(kv_data_store_ctx arg, bool success) {
    struct set_ctx *ctx = arg;
    struct kv_bucket_segment *seg;
    TAILQ_FOREACH(seg, &ctx->segs, entry) {
        if (success) kv_bucket_seg_commit(&ctx->self->bucket_log, seg);
    }
    kv_bucket_unlock(&ctx->self->bucket_log, &ctx->segs);
    kv_free(ctx);
}

static void set_finish_cb(bool success, void *arg) {
    struct set_ctx *ctx = arg;
    success = ctx->success && success;
    if (--ctx->io_cnt) return;  // sync
    if (ctx->cb) ctx->cb(success, ctx->cb_arg);
}

static void set_lock_cb(void *arg) {
    struct set_ctx *ctx = arg;
    if (ctx->success == false) {
        set_finish_cb(false, arg);
        return;
    }
    struct kv_item *located_item;
    find_item_plus(ctx->self, &ctx->seg, ctx->key, ctx->key_length, &located_item);
    if (located_item) {  // update
        ctx->seg.dirty = true;
        located_item->value_length = ctx->value_length;
        located_item->value_offset = ctx->value_offset;
        kv_bucket_seg_put(&ctx->self->bucket_log, &ctx->seg, set_finish_cb, ctx);
    } else {  // create
        if ((located_item = find_empty(ctx->self, &ctx->seg))) {
            ctx->seg.dirty = true;
            located_item->key_length = ctx->key_length;
            kv_memcpy(located_item->key, ctx->key, ctx->key_length);
            located_item->value_length = ctx->value_length;
            located_item->value_offset = ctx->value_offset;
            kv_bucket_seg_put(&ctx->self->bucket_log, &ctx->seg, set_finish_cb, ctx);
        } else {
            fprintf(stderr, "set_find_item_cb: No more bucket available.\n");
            set_finish_cb(false, arg);
        }
    }
}

static void buffered_set_lock_cb(void *arg) {
    struct set_ctx *ctx = arg;
    if (ctx->success == false) {
        set_finish_cb(false, arg);
        return;
    }
    for (uint32_t i = 0; i < ctx->set_ctx_buffer.buffer_size; ++i) {
        struct kv_bucket_segment *seg;
        TAILQ_FOREACH(seg, &ctx->segs, entry) {
            if (seg->bucket_id == ctx->set_ctx_buffer.bucket_id[i]) {
                break;
            }
        }
        assert(seg);
        struct kv_item *located_item;
        find_item_plus(ctx->self, seg, ctx->set_ctx_buffer.key[i], ctx->set_ctx_buffer.key_length[i], &located_item);
        if (located_item) {  // update
            seg->dirty = true;
            located_item->value_length = ctx->set_ctx_buffer.value_length[i];
            located_item->value_offset = ctx->set_ctx_buffer.value_offset[i];
        } else {  // create
            if ((located_item = find_empty(ctx->self, seg))) {
                ctx->seg.dirty = true;
                located_item->key_length = ctx->set_ctx_buffer.key_length[i];
                kv_memcpy(located_item->key, ctx->set_ctx_buffer.key[i], ctx->set_ctx_buffer.key_length[i]);
                located_item->value_length = ctx->set_ctx_buffer.value_length[i];
                located_item->value_offset = ctx->set_ctx_buffer.value_offset[i];
            } else {
                set_finish_cb(false, arg);
                return;
            }
        }
    }
    kv_bucket_seg_put_bulk(&ctx->self->bucket_log, &ctx->segs, set_finish_cb, ctx);
}

static void set_start(void *arg) {
    struct set_ctx *ctx = arg;
    ctx->io_cnt = 2;
    ctx->success = true;
    ctx->value_offset = kv_value_log_offset(&ctx->self->value_log);
    kv_value_log_write(&ctx->self->value_log, ctx->bucket_id, ctx->value, ctx->value_length, set_finish_cb, ctx);

    TAILQ_INIT(&ctx->segs);
    kv_bucket_seg_init(&ctx->seg, ctx->bucket_id);
    TAILQ_INSERT_HEAD(&ctx->segs, &ctx->seg, entry);
    kv_bucket_lock(&ctx->self->bucket_log, &ctx->segs, set_lock_cb, ctx);
}

kv_data_store_ctx kv_data_store_set(struct kv_data_store *self, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t value_length,
                                    kv_data_store_cb cb, void *cb_arg) {
    struct set_ctx *ctx = kv_malloc(sizeof(struct set_ctx));
    *ctx = (struct set_ctx){self, key, key_length, value, value_length, cb, cb_arg};
    ctx->bucket_id = kv_data_store_bucket_id(self, key);
    ctx->cb = dequeue;
    ctx->cb_arg = enqueue(self, KV_DS_SET, set_start, ctx, cb, cb_arg);
    return ctx;
}

static void buffered_set_start(void *arg) {
    struct set_ctx *ctx = arg;
    ctx->io_cnt = 2;
    ctx->success = true;
    kv_value_log_buffered_offset(&ctx->self->value_log, ctx->set_ctx_buffer.value_length, ctx->set_ctx_buffer.value_offset, ctx->set_ctx_buffer.buffer_size);
    kv_value_log_buffered_write(&ctx->self->value_log, ctx->set_ctx_buffer.value_offset, ctx->set_ctx_buffer.bucket_id, ctx->set_ctx_buffer.value, ctx->set_ctx_buffer.value_length, ctx->set_ctx_buffer.buffer_size, set_finish_cb, ctx);

    TAILQ_INIT(&ctx->segs);
    for (uint32_t i = 0; i < ctx->set_ctx_buffer.buffer_size; ++i) {
        for (uint32_t j = 0; j < i; ++j) {
            if (ctx->set_ctx_buffer.bucket_id[j] == ctx->set_ctx_buffer.bucket_id[i]) {
                goto collision;
            }
        }
        kv_bucket_seg_init(&ctx->set_ctx_buffer.seg[i], ctx->set_ctx_buffer.bucket_id[i]);
        TAILQ_INSERT_HEAD(&ctx->segs, &ctx->set_ctx_buffer.seg[i], entry);
        collision:;
    }
    kv_bucket_lock(&ctx->self->bucket_log, &ctx->segs, buffered_set_lock_cb, ctx);
}

kv_data_store_ctx kv_data_store_buffered_set(struct kv_data_store *self, uint8_t *key[], uint8_t key_length[], uint8_t *value[], uint32_t value_length[],
                                             uint64_t value_offset[], uint64_t bucket_id[], struct kv_bucket_segment seg[], uint32_t buffer_size,
                                             kv_data_store_cb cb, void *cb_arg) {
    struct set_ctx *ctx = kv_malloc(sizeof(struct set_ctx));
    ctx->self = self;
    ctx->set_ctx_buffer.key = key;
    ctx->set_ctx_buffer.key_length = key_length;
    ctx->set_ctx_buffer.value_length = value_length;
    ctx->set_ctx_buffer.value_offset = value_offset;
    ctx->set_ctx_buffer.bucket_id = bucket_id;
    ctx->set_ctx_buffer.seg = seg;
    ctx->set_ctx_buffer.buffer_size = buffer_size;
    uint64_t offset = 0;
    for (uint32_t i = 0; i < ctx->set_ctx_buffer.buffer_size; ++i) {
        ctx->set_ctx_buffer.bucket_id[i] = kv_data_store_bucket_id(self, key[i]);
        memcpy(ctx->set_ctx_buffer.value + offset, value[i], value_length[i]);
        offset += value_length[i];
    }
    ctx->cb = dequeue;
    ctx->cb_arg = enqueue(self, KV_DS_SET, buffered_set_start, ctx, cb, cb_arg);
    return ctx;
}
// --- get ---
struct get_ctx {
    struct kv_data_store *self;
    uint8_t *key;
    uint8_t key_length;
    uint8_t *value;
    uint32_t *value_length;
    kv_data_store_cb cb;
    void *cb_arg;
    struct kv_bucket_segment seg;
    struct kv_bucket_meta *meta;
};

static void get_seg_cb(bool success, void *arg) {
    struct get_ctx *ctx = arg;
    if (success) {
        struct kv_item *located_item;
        find_item_plus(ctx->self, &ctx->seg, ctx->key, ctx->key_length, &located_item);
        if (located_item) {
            *ctx->value_length = located_item->value_length;
            kv_value_log_read(&ctx->self->value_log, located_item->value_offset, ctx->value, located_item->value_length, ctx->cb,
                              ctx->cb_arg);
        } else {
            success = false;
        }
    }
    if (!success && ctx->cb) ctx->cb(false, ctx->cb_arg);
    kv_bucket_seg_cleanup(&ctx->self->bucket_log, &ctx->seg);
    kv_free(ctx);
}

static void get_read_bucket(void *arg) {
    struct get_ctx *ctx = arg;
    kv_bucket_seg_init(&ctx->seg, kv_data_store_bucket_id(ctx->self, ctx->key));
    kv_bucket_seg_get(&ctx->self->bucket_log, &ctx->seg, ctx->meta, ctx->meta ? false : true, get_seg_cb, ctx);
}

void kv_data_store_get(struct kv_data_store *self, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t *value_length,
                       struct kv_bucket_meta *meta, kv_data_store_cb cb, void *cb_arg) {
    struct get_ctx *ctx = kv_malloc(sizeof(struct get_ctx));
    *ctx = (struct get_ctx){self, key, key_length, value, value_length};
    ctx->meta = meta;
    ctx->cb = dequeue;
    ctx->cb_arg = enqueue(self, KV_DS_GET, get_read_bucket, ctx, cb, cb_arg);
}

// --- delete ---
struct delete_ctx {
    struct kv_data_store *self;
    uint8_t *key;
    uint8_t key_length;
    kv_data_store_cb cb;
    void *cb_arg;
    uint64_t bucket_id;
    struct kv_bucket_segments segs;
    struct kv_bucket_segment seg;
    bool success;
};

void kv_data_store_del_commit(kv_data_store_ctx arg, bool success) {
    struct delete_ctx *ctx = arg;
    if (success) kv_bucket_seg_commit(&ctx->self->bucket_log, &ctx->seg);
    kv_bucket_unlock(&ctx->self->bucket_log, &ctx->segs);
    kv_free(ctx);
}

static void delete_finish_cb(bool success, void *arg) {
    struct delete_ctx *ctx = arg;
    if (ctx->cb) ctx->cb(success, ctx->cb_arg);
}

static void delete_lock_cb(void *arg) {
    struct delete_ctx *ctx = arg;
    struct kv_item *located_item = NULL;
    find_item_plus(ctx->self, &ctx->seg, ctx->key, ctx->key_length, &located_item);
    if (!located_item) {
        delete_finish_cb(false, arg);
        return;
    }
    ctx->seg.dirty = true;
    located_item->key_length = 0;
    fill_the_hole(ctx->self, &ctx->seg);
    kv_bucket_seg_put(&ctx->self->bucket_log, &ctx->seg, delete_finish_cb, ctx);
}

static void delete_lock(void *arg) {
    struct delete_ctx *ctx = arg;
    TAILQ_INIT(&ctx->segs);
    kv_bucket_seg_init(&ctx->seg, ctx->bucket_id);
    TAILQ_INSERT_HEAD(&ctx->segs, &ctx->seg, entry);
    kv_bucket_lock(&ctx->self->bucket_log, &ctx->segs, delete_lock_cb, ctx);
}

kv_data_store_ctx kv_data_store_delete(struct kv_data_store *self, uint8_t *key, uint8_t key_length, kv_data_store_cb cb, void *cb_arg) {
    struct delete_ctx *ctx = kv_malloc(sizeof(struct delete_ctx));
    *ctx = (struct delete_ctx){self, key, key_length, cb, cb_arg};
    ctx->bucket_id = kv_data_store_bucket_id(self, key);
    ctx->cb = dequeue;
    ctx->cb_arg = enqueue(self, KV_DS_DEL, delete_lock, ctx, cb, cb_arg);
    return ctx;
}

// --- copy ---
struct key_range_t {
    uint64_t start;
    uint64_t end;
    kv_data_store_cb cb;
    void *cb_arg;
    uint64_t to_copy;
    bool copying;
    struct kv_bucket_segments segs;
    struct kv_bucket_segment seg;
    uint32_t item_num;
    struct copy_ctx_t *ctx;
    CIRCLEQ_ENTRY(key_range_t)
    entry;
    // within a key range at most one bucket segment will be locked.
};

struct copy_read_val_ctx {
    struct kv_data_store_copy_buf buf;
    struct key_range_t *range;
    struct kv_item *item;
    TAILQ_ENTRY(copy_read_val_ctx)
    entry;
};

struct copy_ctx_t {
    struct kv_data_store *self;
    uint32_t buf_num;
    kv_data_store_get_buf_cb get_buf;
    void *arg;
    kv_data_store_cb copy_cb;
    uint32_t iocnt, queue_size;
    struct key_range_t *next_range;  // used by producer
    TAILQ_HEAD(, copy_read_val_ctx)
    queue;
    CIRCLEQ_HEAD(, key_range_t)
    key_ranges;
};

static void on_seg_copy_done(struct key_range_t *range) {
    struct copy_ctx_t *ctx = range->ctx;
    range->copying = false;
    kv_bucket_unlock(&ctx->self->bucket_log, &range->segs);
    if (range->to_copy == range->end && range->cb) {
        // printf("%lx %lx\n", range->start, range->end);
        range->cb(true, range->cb_arg);
        range->cb = NULL;
    }
}

static void copy_scheduler(struct copy_ctx_t *ctx);
void kv_data_store_copy_commit(struct kv_data_store_copy_buf *buf) {
    struct copy_read_val_ctx *read_val = (struct copy_read_val_ctx *)buf;
    struct key_range_t *range = read_val->range;
    struct copy_ctx_t *ctx = range->ctx;
    ctx->iocnt--;
    kv_free(read_val);
    if (--range->item_num == 0) on_seg_copy_done(range);
    copy_scheduler(ctx);
}

static void copy_consumer(struct copy_ctx_t *ctx) {
    while (ctx->iocnt < ctx->buf_num) {
        struct copy_read_val_ctx *read_val = TAILQ_FIRST(&ctx->queue);
        if (read_val == NULL) return;
        TAILQ_REMOVE(&ctx->queue, read_val, entry);
        ctx->queue_size--;
        ctx->iocnt++;
        struct kv_item *item = read_val->item;
        read_val->buf.val_len = item->value_length;
        ctx->get_buf(item->key, item->key_length, &read_val->buf, ctx->arg);
        kv_value_log_read(&ctx->self->value_log, item->value_offset, read_val->buf.val_buf, item->value_length,
                          ctx->copy_cb, read_val->buf.ctx);
    }
}

static void copy_lock_cb(void *arg) {
    struct key_range_t *range = arg;
    struct copy_ctx_t *ctx = range->ctx;
    assert(range->seg.empty == false);
    if (TAILQ_EMPTY(&range->seg.chain)) {
        on_seg_copy_done(range);
        copy_scheduler(ctx);
        return;
    }

    struct kv_bucket_chain_entry *ce;
    TAILQ_FOREACH(ce, &range->seg.chain, entry) {
        for (struct kv_bucket *bucket = ce->bucket; bucket - ce->bucket < ce->len; ++bucket)
            for (struct kv_item *item = bucket->items; item - bucket->items < KV_ITEM_PER_BUCKET; ++item) {
                if (KV_EMPTY_ITEM(item)) continue;
                struct copy_read_val_ctx *read_val = kv_malloc(sizeof(*read_val));
                read_val->item = item;
                read_val->range = range;
                range->item_num++;
                ctx->queue_size++;
                TAILQ_INSERT_TAIL(&ctx->queue, read_val, entry);
            }
    }
    copy_scheduler(ctx);
}

static void copy_producer(struct copy_ctx_t *ctx) {
    if (ctx->next_range == NULL) return;
    struct key_range_t *range_base = ctx->next_range;
    while (true) {
        struct key_range_t *range = ctx->next_range;
        uint64_t id_space_size = 1ULL << ctx->self->log_bucket_num;
        if (range->to_copy != range->end && range->copying == false) {
            assert(range->seg.empty);
            kv_bucket_seg_init(&range->seg, range->to_copy);
            assert(range->item_num == 0);
            range->copying = true;
            range->to_copy = (range->to_copy + 1) % id_space_size;
            kv_bucket_lock(&ctx->self->bucket_log, &range->segs, copy_lock_cb, range);
        }
        ctx->next_range = CIRCLEQ_LOOP_NEXT(&ctx->key_ranges, ctx->next_range, entry);
        if (ctx->next_range == range_base) return;
    }
}
static void copy_scheduler(struct copy_ctx_t *ctx) {
    // consumer
    // get a item from the queue -> get buf ->read value-> copy_cb -> unlock
    copy_consumer(ctx);
    // producer
    //  find a none empty bucket -> lock the bucket -> add items to the queue
    if (ctx->queue_size < ctx->buf_num) {
        copy_producer(ctx);
    }
}

bool kv_data_store_copy_forward(struct kv_data_store *self, uint8_t *key) {
    struct copy_ctx_t *ctx = self->copy_ctx;
    uint64_t bucket_id = kv_data_store_bucket_id(self, key), size = 1ULL << ctx->self->log_bucket_num;
    struct key_range_t *i;
    CIRCLEQ_FOREACH(i, &ctx->key_ranges, entry) {
        if ((bucket_id + size - i->start) % size < (i->to_copy + size - i->start) % size)
            return true;
    }
    return false;
}
#define TAILQ_FOREACH_SAFE(var, head, field, tvar) \
    for ((var) = TAILQ_FIRST((head)); (var) && ((tvar) = TAILQ_NEXT((var), field), 1); (var) = (tvar))

void kv_data_store_copy_add_key_range(struct kv_data_store *self, uint8_t *start_key, uint8_t *end_key, kv_data_store_cb cb, void *cb_arg) {
    struct copy_ctx_t *ctx = self->copy_ctx;
    assert(cb);
    struct key_range_t *range = kv_malloc(sizeof(*range));
    *range = (struct key_range_t){kv_data_store_bucket_id(self, start_key), kv_data_store_bucket_id(self, end_key), cb, cb_arg};
    range->ctx = ctx;
    range->copying = false;
    range->to_copy = range->start;
    range->item_num = 0;
    TAILQ_INIT(&range->segs);
    range->seg.empty = true;
    TAILQ_INSERT_TAIL(&range->segs, &range->seg, entry);
    if (ctx->next_range == NULL) {
        assert(CIRCLEQ_EMPTY(&ctx->key_ranges));
        ctx->next_range = range;
    }
    CIRCLEQ_INSERT_HEAD(&ctx->key_ranges, range, entry);
    copy_scheduler(ctx);
}
void kv_data_store_copy_del_key_range(struct kv_data_store *self, uint8_t *start_key, uint8_t *end_key, bool delete_items) {
    struct copy_ctx_t *ctx = self->copy_ctx;
    uint64_t start_id = kv_data_store_bucket_id(self, start_key), end_id = kv_data_store_bucket_id(self, end_key);
    struct key_range_t *range;
    CIRCLEQ_FOREACH(range, &ctx->key_ranges, entry) {
        if (range->start == start_id && range->end == end_id) {
            assert(range->to_copy == range->end && range->copying == false);
            if (ctx->next_range == range) {
                ctx->next_range = CIRCLEQ_LOOP_NEXT(&ctx->key_ranges, range, entry);
                if (ctx->next_range == range) ctx->next_range = NULL;
            }
            if (delete_items) {
                uint64_t id_space_size = 1ULL << ctx->self->log_bucket_num;
                for (uint64_t i = range->start; i != range->end; i = (i + 1) % id_space_size) {
                    kv_bucket_meta_put(&ctx->self->bucket_log, i, (struct kv_bucket_meta){0, 0});
                }
            }
            CIRCLEQ_REMOVE(&ctx->key_ranges, range, entry);
            kv_free(range);
            return;
        }
    }
    assert(false);
}

void kv_data_store_copy_init(struct kv_data_store *self, kv_data_store_get_buf_cb get_buf, void *arg, uint64_t buf_num, kv_data_store_cb cb) {
    struct copy_ctx_t *ctx = kv_malloc(sizeof(*ctx));
    *ctx = (struct copy_ctx_t){self, buf_num, get_buf, arg, cb};
    TAILQ_INIT(&ctx->queue);
    CIRCLEQ_INIT(&ctx->key_ranges);
    ctx->next_range = NULL;
    ctx->iocnt = 0;
    ctx->queue_size = 0;
    self->copy_ctx = ctx;
}

void kv_data_store_copy_fini(struct kv_data_store *self) {
    kv_free(self->copy_ctx);
}