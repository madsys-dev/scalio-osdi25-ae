//
// Created by unidy on 24-6-20.
//

#include <string>
#include "ditto_wrapper.h"
#include "../ditto/experiments/memcached.h"
#include "../ditto/src/third_party/json.hpp"
#include "../ditto/src/client.h"
#include "../ditto/src/server.h"

using json = nlohmann::json;

#define TICK_US (500000)
#define TICK_PER_SEC (1000000 / 500000)

#define MAX_NUM_CLIENTS (256)

DMCMemcachedClient* con_client_list[MAX_NUM_CLIENTS];
DMCClient* client_list[MAX_NUM_CLIENTS];
PackedClient* packed_client_list[MAX_NUM_CLIENTS];
PackedServer* packed_server;
DMCConfig client_conf_list[MAX_NUM_CLIENTS];
DMCConfig server_conf;
pthread_t server_tid;

struct Stat {
    struct timeval st, tst, tet;
    uint32_t seq = 0;
    uint32_t n_miss = 0;
    uint32_t n_get = 0;
    uint32_t n_get_retry = 0;
    uint32_t n_set = 0;
    uint32_t n_set_dummy = 0;
    int tick = 0;
    std::vector<uint32_t> ops_vec{};
    std::vector<uint32_t> n_miss_vec{};
    std::vector<uint64_t> num_evict_vec{};
    std::vector<uint64_t> succ_evict_vec{};
    std::vector<uint64_t> num_evict_bucket_vec{};
    std::vector<uint64_t> succ_evict_bucket_vec{};
    std::vector<float> tmp_weights;
    std::vector<std::vector<float>> adaptive_weight_vec{};
    std::map<uint32_t, uint32_t> lat_map;
} stat_list[MAX_NUM_CLIENTS];

void update_stat(int id, bool miss) {
    auto &stat = stat_list[id];
    gettimeofday(&stat.tet, NULL);
    stat.n_miss += miss;
    stat.seq++;
    stat.lat_map[diff_ts_us(&stat.tet, &stat.tst)]++;
    if ((stat.tet.tv_sec - stat.st.tv_sec) * 1000000 + (stat.tet.tv_usec - stat.st.tv_usec) >
        TICK_US * stat.tick) {
        auto client = client_list[id];
        stat.ops_vec.push_back(stat.seq);
        stat.num_evict_bucket_vec.push_back(client->num_bucket_evict_);
        stat.succ_evict_bucket_vec.push_back(client->num_success_bucket_evict_);
        stat.num_evict_vec.push_back(client->num_evict_);
        stat.succ_evict_vec.push_back(client->num_success_evict_);
        stat.n_miss_vec.push_back(stat.n_miss);
        client->get_adaptive_weights(stat.tmp_weights);
        stat.adaptive_weight_vec.push_back(stat.tmp_weights);
        stat.tick++;
    }
}

void* ditto_sync_ready(void *arg) {
    int id = *(int *) arg;
    con_client_list[id]->memcached_sync_ready(client_conf_list[id].server_id);
    con_client_list[id]->memcached_sync_ready(client_conf_list[id].server_id);
    return nullptr;
}

void ditto_init(int num_clients, int client_id, const char* client_conf_filename, const char* memcached_ip) {
    int arg_list[num_clients];
    pthread_t tid_list[num_clients];
    for (int i = 0; i < num_clients; i++) {
        int ret = load_config(client_conf_filename, &client_conf_list[i]);
        assert(ret == 0);
        client_conf_list[i].server_id = client_id + i;
        client_list[i] = new DMCClient(&client_conf_list[i]);
        con_client_list[i] = new DMCMemcachedClient(memcached_ip);
        arg_list[i] = i;
        pthread_create(&tid_list[i], NULL, ditto_sync_ready, &arg_list[i]);
    }
    for (int i = 0; i < num_clients; i++) {
        pthread_join(tid_list[i], NULL);
        gettimeofday(&stat_list[i].st, NULL);
    }
}

int ditto_get(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t *value_length) {
    stat_list[id].n_get++;
    gettimeofday(&stat_list[id].tst, NULL);
    int ret = client_list[id]->kv_get(key, key_length, value, value_length);
    update_stat(id, ret != 0);
    return ret;
}

int ditto_set(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t value_length) {
    stat_list[id].n_set++;
    gettimeofday(&stat_list[id].tst, NULL);
    int ret = client_list[id]->kv_set(key, key_length, value, value_length);
    update_stat(id, ret != 0);
    return ret;
}

void ditto_fini(int num_clients) {
    uint32_t n_set = 0;
    uint32_t n_get = 0;
    for (int i = 0; i < num_clients; i++) {
        auto &stat = stat_list[i];
        n_set += stat.n_set;
        n_get += stat.n_get;
        auto client = client_list[i];
        json res;
        res["n_ops_cont"] = json(stat.ops_vec);
        res["n_evict_cont"] = json(stat.num_evict_vec);
        res["n_succ_evict_cont"] = json(stat.succ_evict_vec);
        res["n_evict_bucket_cont"] = json(stat.num_evict_bucket_vec);
        res["n_succ_evict_bucket_cont"] = json(stat.succ_evict_bucket_vec);
        res["n_miss_cont"] = json(stat.n_miss_vec);
        res["adaptive_weights_cont"] = json(stat.adaptive_weight_vec);
        res["n_hist_match"] = client->num_hist_match_;
        res["n_get"] = stat.n_get;
        res["n_set"] = stat.n_set;
        res["n_retry"] = client->num_set_retry_;
        res["n_rdma_send"] = client->num_rdma_send_;
        res["n_rdma_recv"] = client->num_rdma_recv_;
        res["n_rdma_read"] = client->num_rdma_read_;
        res["n_rdma_write"] = client->num_rdma_write_;
        res["n_rdma_cas"] = client->num_rdma_cas_;
        res["n_rdma_faa"] = client->num_rdma_faa_;
        res["n_weights_sync"] = client->num_adaptive_weight_sync_;
        res["n_weights_adjust"] = client->num_adaptive_adjust_weights_;
        res["n_read_hist_head"] = client->num_read_hist_head_;
        res["n_hist_overwrite"] = client->num_hist_overwite_;
        res["n_hist_access"] = client->num_hist_access_;
        res["n_ada_evict_inconsistent"] = client->num_ada_evict_inconsistent_;
        res["n_ada_bucket_evict_history"] = client->num_bucket_evict_history_;
        res["n_expert_evict"] = json(client->expert_evict_cnt_);
        res["lat_map"] = json(stat.lat_map);
        std::string str = res.dump();
        con_client_list[i]->memcached_put_result((void*)str.c_str(), strlen(str.c_str()),
                                        client_conf_list[i].server_id);
        delete con_client_list[i];
        delete client_list[i];
    }
    printf("n_set = %d, n_get = %d\n", n_set, n_get);
}

void packed_init(int num_clients, int client_id, const char* client_conf_filename, const char* memcached_ip) {
    int arg_list[num_clients];
    pthread_t tid_list[num_clients];
    for (int i = 0; i < num_clients; i++) {
        int ret = load_config(client_conf_filename, &client_conf_list[i]);
        assert(ret == 0);
        client_conf_list[i].server_id = client_id + i;
        packed_client_list[i] = new PackedClient(&client_conf_list[i]);
        con_client_list[i] = new DMCMemcachedClient(memcached_ip);
        arg_list[i] = i;
        pthread_create(&tid_list[i], NULL, ditto_sync_ready, &arg_list[i]);
    }
    for (int i = 0; i < num_clients; i++) {
        pthread_join(tid_list[i], NULL);
        gettimeofday(&stat_list[i].st, NULL);
    }
}

void packed_fini(int num_clients) {
    uint32_t n_set = 0;
    uint32_t n_set_dummy = 0;
    uint32_t n_get = 0;
    uint32_t n_get_retry = 0;
    for (int i = 0; i < num_clients; i++) {
        auto &stat = stat_list[i];
        n_set += stat.n_set;
        n_set_dummy += stat.n_set_dummy;
        n_get += stat.n_get;
        n_get_retry += stat.n_get_retry;
        json res;
        res["n_ops_cont"] = json(stat.ops_vec);
        res["n_evict_cont"] = json(stat.num_evict_vec);
        res["n_succ_evict_cont"] = json(stat.succ_evict_vec);
        res["n_evict_bucket_cont"] = json(stat.num_evict_bucket_vec);
        res["n_succ_evict_bucket_cont"] = json(stat.succ_evict_bucket_vec);
        res["n_miss_cont"] = json(stat.n_miss_vec);
        res["adaptive_weights_cont"] = json(stat.adaptive_weight_vec);
        res["n_hist_match"] = 0;
        res["n_get"] = stat.n_get;
        res["n_set"] = stat.n_set;
        res["n_weights_adjust"] = 0;
        res["lat_map"] = json(stat.lat_map);
        std::string str = res.dump();
        con_client_list[i]->memcached_put_result((void*)str.c_str(), strlen(str.c_str()),
                                                 client_conf_list[i].server_id);
        delete con_client_list[i];
        delete packed_client_list[i];
    }
    printf("n_set = %d, n_set_dummy = %d, n_get = %d, n_get_retry = %d\n", n_set, n_set_dummy, n_get, n_get_retry);
}

void packed_server_init(const char* server_conf_filename) {
    int ret = load_config(server_conf_filename, &server_conf);
    assert(ret == 0);
    packed_server = new PackedServer(&server_conf);
    ret = pthread_create(&server_tid, NULL, packed_server_main, packed_server);
    assert(ret == 0);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(server_conf.core_id, &cpuset);
    printf("Server is running on core: %d\n", server_conf.core_id);

    ret = pthread_setaffinity_np(server_tid, sizeof(cpu_set_t), &cpuset);
    assert(ret == 0);
}

void packed_server_fini() {
    packed_server->stop();
    pthread_join(server_tid, NULL);
    delete packed_server;
}

void packed_update_stat(int id, bool miss) {
    auto &stat = stat_list[id];
    gettimeofday(&stat.tet, NULL);
    stat.n_miss += miss;
    stat.seq++;
    stat.lat_map[diff_ts_us(&stat.tet, &stat.tst)]++;
    if ((stat.tet.tv_sec - stat.st.tv_sec) * 1000000 + (stat.tet.tv_usec - stat.st.tv_usec) >
        TICK_US * stat.tick) {
        stat.ops_vec.push_back(stat.seq);
        stat.num_evict_bucket_vec.push_back(0);
        stat.succ_evict_bucket_vec.push_back(0);
        stat.num_evict_vec.push_back(0);
        stat.succ_evict_vec.push_back(0);
        stat.n_miss_vec.push_back(stat.n_miss);
        stat.adaptive_weight_vec.push_back(stat.tmp_weights);
        stat.tick++;
    }
}

int packed_get(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t *value_length) {
    stat_list[id].n_get++;
    gettimeofday(&stat_list[id].tst, NULL);
    int ret = packed_client_list[id]->kv_get(key, key_length, value, value_length);
    packed_update_stat(id, ret != 0);
    return ret;
}

int packed_get_retry(int id) {
    stat_list[id].n_get_retry++;
    return 0;
}

int packed_set(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t value_length, uint8_t slot_id) {
    stat_list[id].n_set++;
    gettimeofday(&stat_list[id].tst, NULL);
    int ret = packed_client_list[id]->kv_set(key, key_length, value, value_length, slot_id);
    packed_update_stat(id, ret != 0);
    return ret;
}

int packed_set_dummy(int id) {
    stat_list[id].n_set_dummy++;
    return -1;
}

int packed_server_put_key(uint8_t *key, uint8_t key_length) {
    return packed_server->put_key(key, key_length);
}

int packed_server_invalidate_key(uint8_t *key, uint8_t key_length) {
    return packed_server->invalidate_key(key, key_length);
}
