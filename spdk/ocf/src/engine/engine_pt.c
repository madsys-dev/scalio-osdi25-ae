/*
 * Copyright(c) 2012-2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "engine_pt.h"
#include "engine_common.h"
#include "cache_engine.h"
#include "../ocf_request.h"
#include "../utils/utils_io.h"
#include "../utils/utils_part.h"
#include "../metadata/metadata.h"
#include "../concurrency/ocf_concurrency.h"

#define OCF_ENGINE_DEBUG_IO_NAME "pt"
#include "engine_debug.h"

static void _ocf_read_pt_complete(struct ocf_request *req, int error)
{
	if (error)
		req->error |= error;

	if (env_atomic_dec_return(&req->req_remaining))
		return;

	OCF_DEBUG_RQ(req, "Completion");

	if (req->error) {
		req->info.core_error = 1;
		env_atomic_inc(&req->core->counters->core_errors.read);
	}

	/* Complete request */
	req->complete(req, req->error);

	ocf_req_unlock_rd(req);

	/* Release OCF request */
	ocf_req_put(req);
}

static inline void _ocf_read_pt_submit(struct ocf_request *req)
{
	env_atomic_set(&req->req_remaining, 1); /* Core device IO */

	OCF_DEBUG_RQ(req, "Submit");

	/* Core read */
	ocf_submit_volume_req(&req->core->volume, req, _ocf_read_pt_complete);
}

int ocf_read_pt_do(struct ocf_request *req)
{
	struct ocf_cache *cache = req->cache;

	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	if (req->info.dirty_any) {
		OCF_METADATA_LOCK_RD();
		/* Need to clean, start it */
		ocf_engine_clean(req);
		OCF_METADATA_UNLOCK_RD();

		/* Do not processing, because first we need to clean request */
		ocf_req_put(req);

		return 0;
	}

	if (req->info.re_part) {
		OCF_DEBUG_RQ(req, "Re-Part");

		OCF_METADATA_LOCK_WR();

		/* Probably some cache lines are assigned into wrong
		 * partition. Need to move it to new one
		 */
		ocf_part_move(req);

		OCF_METADATA_UNLOCK_WR();
	}

	/* Submit read IO to the core */
	_ocf_read_pt_submit(req);

	/* Update statistics */
	ocf_engine_update_block_stats(req);
	env_atomic64_inc(&req->core->counters->
			part_counters[req->part_id].read_reqs.pass_through);

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return 0;
}

static const struct ocf_io_if _io_if_pt_resume = {
	.read = ocf_read_pt_do,
	.write = ocf_read_pt_do,
	.resume = ocf_engine_on_resume,
};

int ocf_read_pt(struct ocf_request *req)
{
	bool use_cache = false;
	int lock = OCF_LOCK_NOT_ACQUIRED;
	struct ocf_cache *cache = req->cache;

	OCF_DEBUG_TRACE(req->cache);

	ocf_io_start(req->io);

	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	/* Set resume io_if */
	req->io_if = &_io_if_pt_resume;

	OCF_METADATA_LOCK_RD(); /*- Metadata RD access -----------------------*/

	/* Traverse request to check if there are mapped cache lines */
	ocf_engine_traverse(req);

	if (req->info.seq_cutoff && ocf_engine_is_dirty_all(req)) {
		use_cache = true;
	} else {
		if (ocf_engine_mapped_count(req)) {
			/* There are mapped cache line,
			 * lock request for READ access
			 */
			lock = ocf_req_trylock_rd(req);
		} else {
			/* No mapped cache lines, no need to get lock */
			lock = OCF_LOCK_ACQUIRED;
		}
	}

	OCF_METADATA_UNLOCK_RD(); /*- END Metadata RD access -----------------*/

	if (use_cache) {
		/*
		 * There is dirt HIT, and sequential cut off,
		 * because of this force read data from cache
		 */
		ocf_req_clear(req);
		ocf_get_io_if(ocf_cache_mode_wt)->read(req);
	} else {
		if (lock >= 0) {
			if (lock == OCF_LOCK_ACQUIRED) {
				/* Lock acquired perform read off operations */
				ocf_read_pt_do(req);
			} else {
				/* WR lock was not acquired, need to wait for resume */
				OCF_DEBUG_RQ(req, "NO LOCK");
			}
		} else {
			OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
			req->complete(req, lock);
			ocf_req_put(req);
		}
	}

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return 0;
}

void ocf_engine_push_req_front_pt(struct ocf_request *req)
{
	ocf_engine_push_req_front_if(req, &_io_if_pt_resume, true);
}

