#ifndef _KV_RING_H_
#define _KV_RING_H_
#include "kv_rdma.h"
enum { KV_RING_VNODE,
       KV_RING_TAIL,
       KV_RING_COPY };

struct kv_ring_copy_info {
    uint64_t start;
    uint64_t end;
    uint16_t ds_id;
    bool del;
};
typedef void (*kv_ring_cb)(void *arg);
typedef void (*kv_ring_req_handler)(void *req_h, kv_rdma_mr req, void *fwd_ctx, bool has_next_node,
                                    uint32_t ds_id, uint32_t vnode_type, void *arg);
typedef void (*kv_ring_copy_cb)(bool is_start, struct kv_ring_copy_info *info, void *arg);

void kv_ring_dispatch(kv_rdma_mr req, kv_rdma_mr resp, void *resp_addr, kv_ring_cb cb, void *cb_arg);  // for clients
void kv_ring_forward(void *fwd_ctx, kv_rdma_mr req, bool is_copy_req, kv_ring_cb cb, void *cb_arg);    // for servers

void kv_ring_register_copy_cb(kv_ring_copy_cb copy_cb, void *cb_arg);
void kv_ring_stop_copy(struct kv_ring_copy_info *info);
// client: kv_ring_init(kv_rdma_init)
// server: kv_ring_init(kv_rdma_init)->kv_ring_server_init(kv_rdma_listen)
kv_rdma_handle kv_ring_init(char *etcd_ip, char *etcd_port, uint32_t thread_num, kv_ring_cb server_online_cb, void *arg);
void kv_ring_server_init(char *local_ip, char *local_port, uint32_t ring_num, uint32_t vid_per_ssd, uint32_t ds_num,
                         uint32_t rpl_num, uint32_t log_bkt_num, uint32_t con_req_num, uint32_t max_msg_sz, kv_ring_req_handler handler, void *arg,
                         kv_rdma_server_init_cb cb, void *cb_arg);
void kv_ring_fini(kv_rdma_fini_cb cb, void *cb_arg);
#endif