#ifndef _PACKED_DATA_STRUCT_H_
#define _PACKED_DATA_STRUCT_H_

#define PACKED_KEY_LEN (16)
#define PACKED_VALUE_LEN (64)
#define PACKED_HASH_BUCKET_ASSOC_NUM (8)
#define PACKED_HASH_NUM_BUCKETS (280576)

typedef struct __attribute__((__packed__)) _PackedSlot {
    uint8_t visibility;
    uint8_t integrity;
    uint8_t key[PACKED_KEY_LEN];
    uint8_t value[PACKED_VALUE_LEN];
    uint64_t acc_ts;
} PackedSlot;

#define PACKED_SLOT_VISIBILITY_OFF (offsetof(PackedSlot, visibility))
#define PACKED_SLOT_INTEGRITY_OFF (offsetof(PackedSlot, integrity))
#define PACKED_SLOT_KEY_OFF (offsetof(PackedSlot, key))
#define PACKED_SLOT_VALUE_OFF (offsetof(PackedSlot, value))
#define PACKED_SLOT_ACC_TS_OFF (offsetof(PackedSlot, acc_ts))

typedef PackedSlot PackedBucket[PACKED_HASH_BUCKET_ASSOC_NUM];

#endif //_PACKED_DATA_STRUCT_H_
