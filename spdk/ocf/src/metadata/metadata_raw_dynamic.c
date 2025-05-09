/*
 * Copyright(c) 2012-2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "metadata.h"
#include "metadata_hash.h"
#include "metadata_raw.h"
#include "metadata_raw_dynamic.h"
#include "metadata_io.h"
#include "../engine/cache_engine.h"
#include "../engine/engine_common.h"
#include "../utils/utils_io.h"
#include "../ocf_request.h"
#include "../ocf_def_priv.h"
#include "../ocf_priv.h"

#define OCF_METADATA_RAW_DEBUG  0

#if 1 == OCF_METADATA_RAW_DEBUG
#define OCF_DEBUG_TRACE(cache) \
	ocf_cache_log(cache, log_info, "[Metadata][Volatile] %s\n", __func__)

#define OCF_DEBUG_PARAM(cache, format, ...) \
	ocf_cache_log(cache, log_info, "[Metadata][Volatile] %s - "format"\n", \
			__func__, ##__VA_ARGS__)
#else
#define OCF_DEBUG_TRACE(cache)
#define OCF_DEBUG_PARAM(cache, format, ...)
#endif

/*******************************************************************************
 * Common RAW Implementation
 ******************************************************************************/

/*
 * Check if page is valid for specified RAW descriptor
 */
static bool _raw_ssd_page_is_valid(struct ocf_metadata_raw *raw, uint32_t page)
{
	ENV_BUG_ON(page < raw->ssd_pages_offset);
	ENV_BUG_ON(page >= (raw->ssd_pages_offset + raw->ssd_pages));

	return true;
}

/*******************************************************************************
 * RAW dynamic Implementation
 ******************************************************************************/

#define _RAW_DYNAMIC_PAGE(raw, line) \
		((line) / raw->entries_in_page)

#define _RAW_DYNAMIC_PAGE_OFFSET(raw, line) \
		((line % raw->entries_in_page) * raw->entry_size)

/*
 * RAW DYNAMIC control structure
 */
struct _raw_ctrl {
	env_mutex lock;
	env_atomic count;
	void *pages[];
};

static void *_raw_dynamic_get_item(ocf_cache_t cache,
	struct ocf_metadata_raw *raw, ocf_cache_line_t line, uint32_t size)
{
	void *new = NULL;
	struct _raw_ctrl *ctrl = (struct _raw_ctrl *)raw->priv;
	uint32_t page = _RAW_DYNAMIC_PAGE(raw, line);

	ENV_BUG_ON(!_raw_is_valid(raw, line, size));

	OCF_DEBUG_PARAM(cache, "Accessing item %u on page %u", line, page);

	if (!ctrl->pages[page]) {
		/* No page, allocate one, and set*/

		/* This RAW container has some restrictions and need to check
		 * this limitation:
		 * 1. no atomic context when allocation
		 * 2. Only one allocator in time
		 */

		ENV_BUG_ON(env_in_interrupt());

		env_mutex_lock(&ctrl->lock);

		if (ctrl->pages[page]) {
			/* Page has been already allocated, skip allocation */
			goto _raw_dynamic_get_item_SKIP;
		}

		OCF_DEBUG_PARAM(cache, "New page allocation - %u", page);

		new = env_secure_alloc(PAGE_SIZE);
		if (new) {
			ENV_BUG_ON(env_memset(new, PAGE_SIZE, 0));
			ctrl->pages[page] = new;
			env_atomic_inc(&ctrl->count);
		}

_raw_dynamic_get_item_SKIP:

		env_mutex_unlock(&ctrl->lock);
	}

	if (ctrl->pages[page])
		return ctrl->pages[page] + _RAW_DYNAMIC_PAGE_OFFSET(raw, line);

	return NULL;
}

/*
* RAM DYNAMIC Implementation - De-Initialize
*/
int raw_dynamic_deinit(ocf_cache_t cache,
		struct ocf_metadata_raw *raw)
{
	uint32_t i;
	struct _raw_ctrl *ctrl = (struct _raw_ctrl *)raw->priv;

	if (!ctrl)
		return 0;

	OCF_DEBUG_TRACE(cache);

	for (i = 0; i < raw->ssd_pages; i++)
		env_secure_free(ctrl->pages[i], PAGE_SIZE);

	env_vfree(ctrl);
	raw->priv = NULL;

	return 0;
}

/*
 * RAM DYNAMIC Implementation - Initialize
 */
int raw_dynamic_init(ocf_cache_t cache,
		struct ocf_metadata_raw *raw)
{
	struct _raw_ctrl *ctrl;
	size_t size = sizeof(*ctrl) + (sizeof(ctrl->pages[0]) * raw->ssd_pages);

	OCF_DEBUG_TRACE(cache);

	if (raw->entry_size > PAGE_SIZE)
		return -1;

	ctrl = env_vmalloc(size);
	if (!ctrl)
		return -1;

	ENV_BUG_ON(env_memset(ctrl, size, 0));

	if (env_mutex_init(&ctrl->lock)) {
		env_vfree(ctrl);
		return -1;
	}

	raw->priv = ctrl;

	return 0;
}

/*
 * RAW DYNAMIC Implementation - Size of
 */
size_t raw_dynamic_size_of(ocf_cache_t cache,
		struct ocf_metadata_raw *raw)
{
	struct _raw_ctrl *ctrl = (struct _raw_ctrl *)raw->priv;
	size_t size;

	/* Size of allocated items */
	size = env_atomic_read(&ctrl->count);
	size *= PAGE_SIZE;

	/* Size of control structure */
	size += sizeof(*ctrl) + (sizeof(ctrl->pages[0]) * raw->ssd_pages);

	OCF_DEBUG_PARAM(cache, "Count = %d, Size = %lu",
			env_atomic_read(&ctrl->count), size);

	return size;
}

/*
 * RAW DYNAMIC Implementation - Size on SSD
 */
uint32_t raw_dynamic_size_on_ssd(struct ocf_metadata_raw *raw)
{
	const size_t alignment = 128 * KiB / PAGE_SIZE;

	return OCF_DIV_ROUND_UP(raw->ssd_pages, alignment) * alignment;
}

/*
 * RAM DYNAMIC Implementation - Checksum
 */
uint32_t raw_dynamic_checksum(ocf_cache_t cache,
		struct ocf_metadata_raw *raw)
{
	struct _raw_ctrl *ctrl = (struct _raw_ctrl *)raw->priv;
	uint64_t i;
	uint32_t step = 0;
	uint32_t crc = 0;

	for (i = 0; i < raw->ssd_pages; i++) {
		if (ctrl->pages[i])
			crc = env_crc32(crc, ctrl->pages[i], PAGE_SIZE);
		OCF_COND_RESCHED(step, 10000);
	}

	return crc;
}

/*
* RAM DYNAMIC Implementation - Get
*/
int raw_dynamic_get(ocf_cache_t cache,
		struct ocf_metadata_raw *raw, ocf_cache_line_t line,
		void *data, uint32_t size)
{
	void *item = _raw_dynamic_get_item(cache, raw, line, size);

	if (!item) {
		ENV_BUG_ON(env_memset(data, size, 0));
		ocf_metadata_error(cache);
		return -1;
	}

	return env_memcpy(data, size, item, size);
}

/*
* RAM DYNAMIC Implementation - Set
*/
int raw_dynamic_set(ocf_cache_t cache,
		struct ocf_metadata_raw *raw, ocf_cache_line_t line,
		void *data, uint32_t size)
{
	void *item = _raw_dynamic_get_item(cache, raw, line, size);

	if (!item) {
		ocf_metadata_error(cache);
		return -1;
	}

	return env_memcpy(item, size, data, size);
}

/*
* RAM DYNAMIC Implementation - access
*/
const void *raw_dynamic_rd_access(ocf_cache_t cache,
		struct ocf_metadata_raw *raw, ocf_cache_line_t line,
		uint32_t size)
{
	return _raw_dynamic_get_item(cache, raw, line, size);
}

/*
* RAM DYNAMIC Implementation - access
*/
void *raw_dynamic_wr_access(ocf_cache_t cache,
		struct ocf_metadata_raw *raw, ocf_cache_line_t line,
		uint32_t size)
{
	return _raw_dynamic_get_item(cache, raw, line, size);
}

/*
* RAM DYNAMIC Implementation - Load all
*/
#define RAW_DYNAMIC_LOAD_PAGES 128

struct raw_dynamic_load_all_context {
	struct ocf_metadata_raw *raw;
	struct ocf_request *req;
	ocf_cache_t cache;
	struct ocf_io *io;
	ctx_data_t *data;
	uint8_t *zpage;
	uint8_t *page;
	uint64_t i;
	int error;

	ocf_metadata_end_t cmpl;
	void *priv;
};

static void raw_dynamic_load_all_complete(
		struct raw_dynamic_load_all_context *context, int error)
{
	context->cmpl(context->priv, error);

	ocf_req_put(context->req);
	env_secure_free(context->page, PAGE_SIZE);
	env_free(context->zpage);
	ctx_data_free(context->cache->owner, context->data);
	env_vfree(context);
}

static int raw_dynamic_load_all_update(struct ocf_request *req);

static const struct ocf_io_if _io_if_raw_dynamic_load_all_update = {
	.read = raw_dynamic_load_all_update,
	.write = raw_dynamic_load_all_update,
};

static void raw_dynamic_load_all_read_end(struct ocf_io *io, int error)
{
	struct raw_dynamic_load_all_context *context = io->priv1;

	ocf_io_put(io);

	if (error) {
		raw_dynamic_load_all_complete(context, error);
		return;
	}

	context->req->io_if = &_io_if_raw_dynamic_load_all_update;
	ocf_engine_push_req_front(context->req, true);
}

static int raw_dynamic_load_all_read(struct ocf_request *req)
{
	struct raw_dynamic_load_all_context *context = req->priv;
	struct ocf_metadata_raw *raw = context->raw;
	uint64_t count;
	int result;

	count = OCF_MIN(RAW_DYNAMIC_LOAD_PAGES, raw->ssd_pages - context->i);

	/* Allocate IO */
	context->io = ocf_new_cache_io(context->cache);
	if (!context->io) {
		raw_dynamic_load_all_complete(context, -OCF_ERR_NO_MEM);
		return 0;
	}

	/* Setup IO */
	result = ocf_io_set_data(context->io, context->data, 0);
	if (result) {
		ocf_io_put(context->io);
		raw_dynamic_load_all_complete(context, result);
		return 0;
	}
	ocf_io_configure(context->io,
		PAGES_TO_BYTES(raw->ssd_pages_offset + context->i),
		PAGES_TO_BYTES(count), OCF_READ, 0, 0);

	ocf_io_set_queue(context->io, req->io_queue);
	ocf_io_set_cmpl(context->io, context, NULL,
			raw_dynamic_load_all_read_end);

	/* Submit IO */
	ocf_volume_submit_io(context->io);

	return 0;
}

static const struct ocf_io_if _io_if_raw_dynamic_load_all_read = {
	.read = raw_dynamic_load_all_read,
	.write = raw_dynamic_load_all_read,
};

static int raw_dynamic_load_all_update(struct ocf_request *req)
{
	struct raw_dynamic_load_all_context *context = req->priv;
	struct ocf_metadata_raw *raw = context->raw;
	struct _raw_ctrl *ctrl = (struct _raw_ctrl *)raw->priv;
	ocf_cache_t cache = context->cache;
	uint64_t count = BYTES_TO_PAGES(context->io->bytes);
	uint64_t i_page;
	int result = 0;
	int cmp;

	/* Reset head of data buffer */
	ctx_data_seek_check(context->cache->owner, context->data,
			ctx_data_seek_begin, 0);

	for (i_page = 0; i_page < count; i_page++, context->i++) {
		if (!context->page) {
			context->page = env_secure_alloc(PAGE_SIZE);
			if (!context->page) {
				/* Allocation error */
				result = -OCF_ERR_NO_MEM;
				break;
			}
		}

		ctx_data_rd_check(cache->owner, context->page,
				context->data, PAGE_SIZE);

		result = env_memcmp(context->zpage, PAGE_SIZE, context->page,
				PAGE_SIZE, &cmp);
		if (result)
			break;

		/* When page is zero set, no need to allocate space for it */
		if (cmp == 0) {
			OCF_DEBUG_PARAM(cache, "Zero loaded %llu", i);
			continue;
		}

		OCF_DEBUG_PARAM(cache, "Non-zero loaded %llu", i);

		if (ctrl->pages[context->i])
			env_vfree(ctrl->pages[context->i]);

		ctrl->pages[context->i] = context->page;
		context->page = NULL;

		env_atomic_inc(&ctrl->count);
	}

	if (result || context->i >= raw->ssd_pages) {
		raw_dynamic_load_all_complete(context, result);
		return 0;
	}

	context->req->io_if = &_io_if_raw_dynamic_load_all_read;
	ocf_engine_push_req_front(context->req, true);

	return 0;
}

void raw_dynamic_load_all(ocf_cache_t cache, struct ocf_metadata_raw *raw,
		ocf_metadata_end_t cmpl, void *priv)
{
	struct raw_dynamic_load_all_context *context;
	int result;

	OCF_DEBUG_TRACE(cache);

	context = env_vzalloc(sizeof(*context));
	if (!context)
		OCF_CMPL_RET(priv, -OCF_ERR_NO_MEM);

	context->raw = raw;
	context->cache = cache;
	context->cmpl = cmpl;
	context->priv = priv;

	context->data = ctx_data_alloc(cache->owner, RAW_DYNAMIC_LOAD_PAGES);
	if (!context->data) {
		result = -OCF_ERR_NO_MEM;
		goto err_data;
	}

	context->zpage = env_zalloc(PAGE_SIZE, ENV_MEM_NORMAL);
	if (!context->zpage) {
		result = -OCF_ERR_NO_MEM;
		goto err_zpage;
	}

	context->req = ocf_req_new(cache->mngt_queue, NULL, 0, 0, 0);
	if (!context->req) {
		result = -OCF_ERR_NO_MEM;
		goto err_req;
	}

	context->req->info.internal = true;
	context->req->priv = context;
	context->req->io_if = &_io_if_raw_dynamic_load_all_read;

	ocf_engine_push_req_front(context->req, true);
	return;

err_req:
	env_free(context->zpage);
err_zpage:
	ctx_data_free(cache->owner, context->data);
err_data:
	env_vfree(context);
	OCF_CMPL_RET(priv, result);
}

/*
 * RAM DYNAMIC Implementation - Flush all
 */

struct raw_dynamic_flush_all_context {
	struct ocf_metadata_raw *raw;
	ocf_metadata_end_t cmpl;
	void *priv;
};

/*
 * RAM Implementation - Flush IO callback - Fill page
 */
static int raw_dynamic_flush_all_fill(ocf_cache_t cache,
		ctx_data_t *data, uint32_t page, void *priv)
{
	struct raw_dynamic_flush_all_context *context = priv;
	struct ocf_metadata_raw *raw = context->raw;
	struct _raw_ctrl *ctrl = (struct _raw_ctrl *)raw->priv;
	uint32_t raw_page;

	ENV_BUG_ON(!_raw_ssd_page_is_valid(raw, page));

	raw_page = page - raw->ssd_pages_offset;

	if (ctrl->pages[raw_page]) {
		OCF_DEBUG_PARAM(cache, "Page = %u", raw_page);
		ctx_data_wr_check(cache->owner, data, ctrl->pages[raw_page],
				PAGE_SIZE);
	} else {
		OCF_DEBUG_PARAM(cache, "Zero fill, Page = %u", raw_page);
		/* Page was not allocated before set only zeros */
		ctx_data_zero_check(cache->owner, data, PAGE_SIZE);
	}

	return 0;
}

static void raw_dynamic_flush_all_complete(ocf_cache_t cache,
		void *priv, int error)
{
	struct raw_dynamic_flush_all_context *context = priv;

	context->cmpl(context->priv, error);
	env_vfree(context);
}

void raw_dynamic_flush_all(ocf_cache_t cache, struct ocf_metadata_raw *raw,
		ocf_metadata_end_t cmpl, void *priv)
{
	struct raw_dynamic_flush_all_context *context;
	int result;

	OCF_DEBUG_TRACE(cache);

	context = env_vmalloc(sizeof(*context));
	if (!context)
		OCF_CMPL_RET(priv, -OCF_ERR_NO_MEM);

	context->raw = raw;
	context->cmpl = cmpl;
	context->priv = priv;

	result = metadata_io_write_i_asynch(cache, cache->mngt_queue, context,
			raw->ssd_pages_offset, raw->ssd_pages,
			raw_dynamic_flush_all_fill,
			raw_dynamic_flush_all_complete);
	if (result)
		OCF_CMPL_RET(priv, result);
}

/*
 * RAM DYNAMIC Implementation - Mark to Flush
 */
void raw_dynamic_flush_mark(ocf_cache_t cache, struct ocf_request *req,
		uint32_t map_idx, int to_state, uint8_t start, uint8_t stop)
{
	ENV_BUG();
}

/*
 * RAM DYNAMIC Implementation - Do flushing asynchronously
 */
int raw_dynamic_flush_do_asynch(ocf_cache_t cache, struct ocf_request *req,
		struct ocf_metadata_raw *raw, ocf_req_end_t complete)
{
	ENV_BUG();
	return -OCF_ERR_NOT_SUPP;
}
