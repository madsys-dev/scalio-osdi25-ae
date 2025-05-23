/*
 * Copyright(c) 2019 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "cache_engine.h"
#include "engine_common.h"
#include "engine_rd.h"
#include "engine_pt.h"
#include "../metadata/metadata.h"
#include "../utils/utils_io.h"
#include "../utils/utils_cache_line.h"
#include "../utils/utils_part.h"
#include "../concurrency/ocf_concurrency.h"

#define OCF_ENGINE_DEBUG_IO_NAME "wo"
#include "engine_debug.h"

static void ocf_read_wo_cache_complete(struct ocf_request *req, int error)
{
	if (error) {
		env_atomic_inc(&req->core->counters->cache_errors.read);
		req->error |= error;
	}

	if (env_atomic_dec_return(&req->req_remaining))
		return;

	OCF_DEBUG_RQ(req, "Completion");

	if (req->error)
		ocf_engine_error(req, true, "Failed to read data from cache");

	ocf_req_unlock_rd(req);

	/* Complete request */
	req->complete(req, req->error);

	/* Release OCF request */
	ocf_req_put(req);
}

static void ocf_read_wo_cache_io(struct ocf_request *req, uint64_t offset,
		uint64_t size)
{
	OCF_DEBUG_RQ(req, "Submit cache");
	env_atomic_inc(&req->req_remaining);
	ocf_submit_cache_reqs(req->cache, req, OCF_READ, offset, size, 1,
			ocf_read_wo_cache_complete);
}

static int ocf_read_wo_cache_do(struct ocf_request *req)
{
	ocf_cache_t cache = req->cache;
	uint32_t s, e, i;
	uint64_t line;
	struct ocf_map_info *entry;
	bool dirty = false;
	bool io = false;
	uint64_t phys_prev, phys_curr = 0;
	uint64_t io_start = 0;
	uint64_t offset = 0;
	uint64_t increment = 0;

	env_atomic_set(&req->req_remaining, 1);

	for (line = 0; line < req->core_line_count; ++line) {
		entry = &req->map[line];
		s = ocf_map_line_start_sector(req, line);
		e = ocf_map_line_end_sector(req, line);

		/* if cacheline mapping is not sequential, send cache IO to
		 * previous cacheline(s) */
		phys_prev = phys_curr;
		if (entry->status != LOOKUP_MISS)
			phys_curr = ocf_metadata_map_lg2phy(cache,
					entry->coll_idx);
		if (io && phys_prev + 1 != phys_curr) {
			ocf_read_wo_cache_io(req, io_start, offset - io_start);
			io = false;
		}

		/* try to seek directly to the last sector */
		if (entry->status == LOOKUP_MISS ||
				ocf_engine_map_all_sec_clean(req, line)) {
			/* all sectors invalid or clean */
			i = e + 1;
			increment = SECTORS_TO_BYTES(e - s + 1);
			dirty = false;
		}
		else if (ocf_engine_map_all_sec_dirty(req, line)) {
			/* all sectors dirty */
			i = e + 1;
			increment = SECTORS_TO_BYTES(e - s + 1);
			dirty = true;
		} else {
			/* need to iterate through CL sector by sector */
			i = s;
		}

		do {
			if (i <= e) {
				 dirty = metadata_test_dirty_one(cache,
						entry->coll_idx, i);
				 increment = 0;
				 do {
					++i;
					increment += SECTORS_TO_BYTES(1);
				 } while (i <= e && metadata_test_dirty_one(
						cache, entry->coll_idx, i)
						== dirty);
			}

			if (io && !dirty) {
				/* end of sequential dirty region */
				ocf_read_wo_cache_io(req, io_start,
						offset - io_start);
				io = false;
			}

			if (!io && dirty) {
				/* beginning of sequential dirty region */
				io = true;
				io_start = offset;
			}

			offset += increment;
		} while (i <= e);
	}

	if (io)
		ocf_read_wo_cache_io(req, io_start, offset - io_start);

	ocf_read_wo_cache_complete(req, 0);

	return 0;
}

static const struct ocf_io_if _io_if_wo_cache_read = {
	.read = ocf_read_wo_cache_do,
	.write = ocf_read_wo_cache_do,
};

static void _ocf_read_wo_core_complete(struct ocf_request *req, int error)
{
	if (error) {
		req->error |= error;
		req->info.core_error = 1;
		env_atomic_inc(&req->core->counters->core_errors.read);
	}

	/* if all mapped cachelines are clean, the data we've read from core
	 * is valid and we can complete the request */
	if (!req->info.dirty_any || req->error) {
		OCF_DEBUG_RQ(req, "Completion");
		req->complete(req, req->error);
		ocf_req_unlock_rd(req);
		ocf_req_put(req);
		return;
	}

	req->io_if = &_io_if_wo_cache_read;
	ocf_engine_push_req_front(req, true);
}

int ocf_read_wo_do(struct ocf_request *req)
{
	ocf_req_get(req);

	/* Lack of cacheline repartitioning here is deliberate. WO cache mode
	 * reads should not affect cacheline status as reading data from the
	 * cache is just an internal optimization. Also WO cache mode is
	 * designed to be used with partitioning based on write life-time hints
	 * and read requests do not carry write lifetime hint by definition.
	 */

	if (ocf_engine_is_hit(req)) {
		/* read hit - just fetch the data from cache */
		OCF_DEBUG_RQ(req, "Submit cache hit");
		ocf_read_generic_submit_hit(req);
	} else {

		OCF_DEBUG_RQ(req, "Submit core");
		ocf_submit_volume_req(&req->core->volume, req,
				_ocf_read_wo_core_complete);
	}

	ocf_engine_update_request_stats(req);
	ocf_engine_update_block_stats(req);

	ocf_req_put(req);
	return 0;
}

static const struct ocf_io_if _io_if_wo_resume = {
	.read = ocf_read_wo_do,
	.write = ocf_read_wo_do,
	.resume = ocf_engine_on_resume,
};

int ocf_read_wo(struct ocf_request *req)
{
	ocf_cache_t cache = req->cache;
	int lock = OCF_LOCK_ACQUIRED;

	OCF_DEBUG_TRACE(req->cache);

	ocf_io_start(req->io);

	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	/* Set resume call backs */
	req->io_if = &_io_if_wo_resume;

	OCF_METADATA_LOCK_RD(); /*- Metadata RD access -----------------------*/

	/* Traverse request to check if there are mapped cache lines */
	ocf_engine_traverse(req);

	if (ocf_engine_mapped_count(req)) {
		/* There are mapped cache lines,
		 * lock request for READ access
		 */
		lock = ocf_req_trylock_rd(req);
	}

	OCF_METADATA_UNLOCK_RD(); /*- END Metadata RD access -----------------*/

	if (lock >= 0) {
		if (lock != OCF_LOCK_ACQUIRED) {
			/* Lock was not acquired, need to wait for resume */
			OCF_DEBUG_RQ(req, "NO LOCK");
		} else {
			/* Lock was acquired can perform IO */
			ocf_read_wo_do(req);
		}
	} else {
		OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
		req->complete(req, lock);
		ocf_req_put(req);
	}

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return 0;
}
