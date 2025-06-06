/*
 * Copyright(c) 2012-2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ocf/ocf.h"
#include "../ocf_priv.h"
#include "../ocf_cache_priv.h"
#include "../ocf_queue_priv.h"
#include "cache_engine.h"
#include "engine_common.h"
#include "engine_rd.h"
#include "engine_wt.h"
#include "engine_pt.h"
#include "engine_wi.h"
#include "engine_wa.h"
#include "engine_wb.h"
#include "engine_wo.h"
#include "engine_fast.h"
#include "engine_discard.h"
#include "engine_d2c.h"
#include "engine_ops.h"
#include "../utils/utils_part.h"
#include "../utils/utils_refcnt.h"
#include "../ocf_request.h"
#include "../metadata/metadata.h"
#include "../eviction/eviction.h"

enum ocf_io_if_type {
	/* Public OCF IO interfaces to be set by user */
	OCF_IO_WT_IF,
	OCF_IO_WB_IF,
	OCF_IO_WA_IF,
	OCF_IO_WI_IF,
	OCF_IO_PT_IF,
	OCF_IO_WO_IF,
	OCF_IO_MAX_IF,

	/* Private OCF interfaces */
	OCF_IO_FAST_IF,
	OCF_IO_DISCARD_IF,
	OCF_IO_D2C_IF,
	OCF_IO_OPS_IF,
	OCF_IO_PRIV_MAX_IF,
};

static const struct ocf_io_if IO_IFS[OCF_IO_PRIV_MAX_IF] = {
	[OCF_IO_WT_IF] = {
		.read = ocf_read_generic,
		.write = ocf_write_wt,
		.name = "Write Through"
	},
	[OCF_IO_WB_IF] = {
		.read = ocf_read_generic,
		.write = ocf_write_wb,
		.name = "Write Back"
	},
	[OCF_IO_WA_IF] = {
		.read = ocf_read_generic,
		.write = ocf_write_wa,
		.name = "Write Around"
	},
	[OCF_IO_WI_IF] = {
		.read = ocf_read_generic,
		.write = ocf_write_wi,
		.name = "Write Invalidate"
	},
	[OCF_IO_PT_IF] = {
		.read = ocf_read_pt,
		.write = ocf_write_wi,
		.name = "Pass Through",
	},
	[OCF_IO_WO_IF] = {
		.read = ocf_read_wo,
		.write = ocf_write_wb,
		.name = "Write Only",
	},
	[OCF_IO_FAST_IF] = {
		.read = ocf_read_fast,
		.write = ocf_write_fast,
		.name = "Fast",
	},
	[OCF_IO_DISCARD_IF] = {
		.read = ocf_discard,
		.write = ocf_discard,
		.name = "Discard",
	},
	[OCF_IO_D2C_IF] =  {
		.read = ocf_io_d2c,
		.write = ocf_io_d2c,
		.name = "Direct to core",
	},
	[OCF_IO_OPS_IF] = {
		.read = ocf_engine_ops,
		.write = ocf_engine_ops,
		.name = "Ops engine",
	},
};

static const struct ocf_io_if *cache_mode_io_if_map[ocf_req_cache_mode_max] = {
	[ocf_req_cache_mode_wt] = &IO_IFS[OCF_IO_WT_IF],
	[ocf_req_cache_mode_wb] = &IO_IFS[OCF_IO_WB_IF],
	[ocf_req_cache_mode_wa] = &IO_IFS[OCF_IO_WA_IF],
	[ocf_req_cache_mode_wi] = &IO_IFS[OCF_IO_WI_IF],
	[ocf_req_cache_mode_wo] = &IO_IFS[OCF_IO_WO_IF],
	[ocf_req_cache_mode_pt] = &IO_IFS[OCF_IO_PT_IF],
	[ocf_req_cache_mode_fast] = &IO_IFS[OCF_IO_FAST_IF],
	[ocf_req_cache_mode_d2c] = &IO_IFS[OCF_IO_D2C_IF],
};

const struct ocf_io_if *ocf_get_io_if(ocf_req_cache_mode_t req_cache_mode)
{
	if (req_cache_mode == ocf_req_cache_mode_max)
		return NULL;
	return cache_mode_io_if_map[req_cache_mode];
}

struct ocf_request *ocf_engine_pop_req(ocf_cache_t cache, ocf_queue_t q)
{
	unsigned long lock_flags = 0;
	struct ocf_request *req;

	OCF_CHECK_NULL(q);

	/* LOCK */
	env_spinlock_lock_irqsave(&q->io_list_lock, lock_flags);

	if (list_empty(&q->io_list)) {
		/* No items on the list */
		env_spinlock_unlock_irqrestore(&q->io_list_lock,
				lock_flags);
		return NULL;
	}

	/* Get the first request and remove it from the list */
	req = list_first_entry(&q->io_list, struct ocf_request, list);

	env_atomic_dec(&q->io_no);
	list_del(&req->list);

	/* UNLOCK */
	env_spinlock_unlock_irqrestore(&q->io_list_lock, lock_flags);

	OCF_CHECK_NULL(req);

	if (ocf_req_alloc_map(req)) {
		req->complete(req, req->error);
		return NULL;
	}

	return req;
}

bool ocf_fallback_pt_is_on(ocf_cache_t cache)
{
	ENV_BUG_ON(env_atomic_read(&cache->fallback_pt_error_counter) < 0);

	return (cache->fallback_pt_error_threshold !=
			OCF_CACHE_FALLBACK_PT_INACTIVE &&
			env_atomic_read(&cache->fallback_pt_error_counter) >=
			cache->fallback_pt_error_threshold);
}

#define SEQ_CUTOFF_FULL_MARGIN \
		(OCF_TO_EVICTION_MIN + OCF_PENDING_EVICTION_LIMIT)

static inline bool ocf_seq_cutoff_is_on(ocf_cache_t cache)
{
	if (!ocf_cache_is_device_attached(cache))
		return false;

	return (cache->device->freelist_part->curr_size <= SEQ_CUTOFF_FULL_MARGIN);
}

bool ocf_seq_cutoff_check(ocf_core_t core, uint32_t dir, uint64_t addr,
		uint64_t bytes)
{
	ocf_cache_t cache = ocf_core_get_cache(core);

	ocf_seq_cutoff_policy policy = ocf_core_get_seq_cutoff_policy(core);

	switch (policy) {
	case ocf_seq_cutoff_policy_always:
		break;

	case ocf_seq_cutoff_policy_full:
		if (ocf_seq_cutoff_is_on(cache))
			break;

	case ocf_seq_cutoff_policy_never:
		return false;
	default:
		ENV_WARN(true, "Invalid sequential cutoff policy!");
		return false;
	}

	if (dir == core->seq_cutoff.rw &&
			core->seq_cutoff.last == addr &&
			core->seq_cutoff.bytes + bytes >=
			ocf_core_get_seq_cutoff_threshold(core)) {
		return true;
	}

	return false;
}

void ocf_seq_cutoff_update(ocf_core_t core, struct ocf_request *req)
{
	/*
	 * If IO is not consequent or has another direction,
	 * reset sequential cutoff state.
	 */
	if (req->byte_position != core->seq_cutoff.last ||
			req->rw != core->seq_cutoff.rw) {
		core->seq_cutoff.rw = req->rw;
		core->seq_cutoff.bytes = 0;
	}

	/* Update last accessed position and bytes counter */
	core->seq_cutoff.last = req->byte_position + req->byte_length;
	core->seq_cutoff.bytes += req->byte_length;
}

ocf_cache_mode_t ocf_get_effective_cache_mode(ocf_cache_t cache,
		ocf_core_t core, struct ocf_io *io)
{
	ocf_cache_mode_t mode;

	if (cache->pt_unaligned_io && !ocf_req_is_4k(io->addr, io->bytes))
		return ocf_cache_mode_pt;

	mode = ocf_part_get_cache_mode(cache,
			ocf_part_class2id(cache, io->io_class));
	if (!ocf_cache_mode_is_valid(mode))
		mode = cache->conf_meta->cache_mode;

	if (ocf_seq_cutoff_check(core, io->dir, io->addr, io->bytes))
		mode = ocf_cache_mode_pt;

	if (ocf_fallback_pt_is_on(cache))
		mode = ocf_cache_mode_pt;

	return mode;
}

int ocf_engine_hndl_req(struct ocf_request *req,
		ocf_req_cache_mode_t req_cache_mode)
{
	ocf_cache_t cache = req->cache;

	OCF_CHECK_NULL(cache);

	req->io_if = ocf_get_io_if(req_cache_mode);
	if (!req->io_if)
		return -OCF_ERR_INVAL;

	/* Till OCF engine is not synchronous fully need to push OCF request
	 * to into OCF workers
	 */

	ocf_engine_push_req_back(req, true);

	return 0;
}

int ocf_engine_hndl_fast_req(struct ocf_request *req,
		ocf_req_cache_mode_t req_cache_mode)
{
	const struct ocf_io_if *io_if;
	int ret;

	io_if = ocf_get_io_if(req_cache_mode);
	if (!io_if)
		return -OCF_ERR_INVAL;

	switch (req->rw) {
	case OCF_READ:
		ret = io_if->read(req);
		break;
	case OCF_WRITE:
		ret = io_if->write(req);
		break;
	default:
		return OCF_FAST_PATH_NO;
	}

	return ret;
}

static void ocf_engine_hndl_2dc_req(struct ocf_request *req)
{
	if (OCF_READ == req->rw)
		IO_IFS[OCF_IO_D2C_IF].read(req);
	else if (OCF_WRITE == req->rw)
		IO_IFS[OCF_IO_D2C_IF].write(req);
	else
		ENV_BUG();
}

void ocf_engine_hndl_discard_req(struct ocf_request *req)
{
	if (req->d2c) {
		ocf_engine_hndl_2dc_req(req);
		return;
	}

	if (OCF_READ == req->rw)
		IO_IFS[OCF_IO_DISCARD_IF].read(req);
	else if (OCF_WRITE == req->rw)
		IO_IFS[OCF_IO_DISCARD_IF].write(req);
	else
		ENV_BUG();
}

void ocf_engine_hndl_ops_req(struct ocf_request *req)
{
	if (req->d2c)
		req->io_if = &IO_IFS[OCF_IO_D2C_IF];
	else
		req->io_if = &IO_IFS[OCF_IO_OPS_IF];

	ocf_engine_push_req_back(req, true);
}
