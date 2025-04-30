//
// Created by unidy on 24-6-20.
//

#ifndef LEED_DITTO_WRAPPER_H
#define LEED_DITTO_WRAPPER_H

#ifdef __cplusplus
extern "C"
{
#endif

    void ditto_init(int num_clients, int client_id, const char* client_conf_filename, const char* memcached_ip);

    int ditto_get(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t *value_length);

    int ditto_set(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t value_length);

    void ditto_fini(int num_clients);

    void packed_init(int num_clients, int client_id, const char* client_conf_filename, const char* memcached_ip);

    int packed_get(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t *value_length);

    int packed_get_retry(int id);

    int packed_set(int id, uint8_t *key, uint8_t key_length, uint8_t *value, uint32_t value_length, uint8_t slot_id);

    int packed_set_dummy(int id);

    void packed_fini(int num_clients);

    void packed_server_init(const char* client_conf_filename);

    void packed_server_fini();

    int packed_server_put_key(uint8_t *key, uint8_t key_length);

    int packed_server_invalidate_key(uint8_t *key, uint8_t key_length);

#ifdef __cplusplus
}
#endif

#endif //LEED_DITTO_WRAPPER_H
