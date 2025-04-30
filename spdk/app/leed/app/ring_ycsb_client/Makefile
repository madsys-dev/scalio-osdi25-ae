
SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = kv_ring_ycsb_client

SYS_LIBS += -lm -lstdc++ -libverbs -lrdmacm -lkv_etcd

CXX_SRCS := ../../utils/concurrentqueue.cpp ../../kv_bucket.cpp ../../ycsb/kv_ycsb.cpp ../../ycsb/core/core_workload.cpp
C_SRCS := ../../kv_storage.c ../../kv_circular_log.c ../../kv_value_log.c ../../kv_bucket_log.c ../../kv_data_store.c ../../kv_memory.c ../../kv_rdma.c ../../kv_ds_queue.c ../../kv_ring.c ../../kv_app.c  ../../utils/city.c ../../utils/timing.c kv_client.c

CXXFLAGS := -DNUM_PQUEUE_SHARDS=32 -DUSE_LOCK_BACKOFF -DUSE_PENALTY -DHASH_BUCKET_ASSOC_NUM=8 -DHASH_NUM_BUCKETS=280576 -I../../ditto/src
LIBS := -lmemcached
CXX_SRCS += ../../ditto/src/client.cc ../../ditto/src/client_mm.cc ../../ditto/src/dmc_table.cc ../../ditto/src/dmc_utils.cc ../../ditto/src/fifo_history.cc ../../ditto/src/ib.cc ../../ditto/src/nm.cc ../../ditto/src/priority.cc ../../ditto/src/rlist.cc ../../ditto/src/server.cc ../../ditto/src/server_mm.cc  ../../ditto/experiments/memcached.cc ../../utils/ditto_wrapper.cpp

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += $(EVENT_BDEV_SUBSYSTEM)
SPDK_LIB_LIST += $(KV_BDEV_MODULES)

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
