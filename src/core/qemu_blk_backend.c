/*
 * QEMU Block Backend for anyfs-reader (synchronous)
 *
 * Uses QEMU's block layer (libblock.a) to support qcow2, vmdk, vdi, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* QEMU headers MUST come before LKL headers to avoid struct iovec conflict.
 * QEMU's osdep.h defines struct iovec on Windows; LKL's lkl_host.h does too. */
#include "block/block-common.h"
#include "block/block-global-state.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/osdep.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "system/block-backend.h"

/* On Windows, QEMU's osdep.h #defines close/open/read/write as wrappers.
 * Undef them now so our struct member names don't get mangled. */
#ifdef _WIN32
#undef close
#undef open
#undef read
#undef write
#endif

/* Prevent LKL from redefining iovec (QEMU already defined it) */
#ifdef _WIN32
/* On Windows, both QEMU (osdep.h) and LKL (lkl_host.h) define struct iovec.
 * Since QEMU is included first, trick LKL into using sys/uio.h path
 * which we provide as an empty file via mingw32 compat headers. */
#define __MSYS__ 1
#endif
#include <lkl.h>
#include <lkl_host.h>
#ifdef _WIN32
#undef __MSYS__
#endif
#include "qemu_blk_backend.h"

struct qemu_blk_ctx {
	BlockBackend* blk;
	int64_t capacity; /* bytes */
};

static int qemu_get_capacity(struct lkl_disk disk, unsigned long long* res)
{
	struct qemu_blk_ctx* ctx = disk.handle;
	*res = ctx->capacity; /* return in bytes, virtio_blk.c divides by 512 */
	return 0;
}

static __thread int qemu_ctx_initialized;

static int qemu_request(struct lkl_disk disk, struct lkl_blk_req* req)
{
	struct qemu_blk_ctx* ctx = disk.handle;
	int64_t offset = (int64_t)req->sector * 512;

	/* Ensure this thread has QEMU's AioContext (LKL may call from any
	 * thread) */
	if (!qemu_ctx_initialized) {
		AioContext* main_ctx = qemu_get_aio_context();
		if (!qemu_get_current_aio_context()) {
			qemu_set_current_aio_context(main_ctx);
		}
		qemu_ctx_initialized = 1;
	}

	for (int i = 0; i < req->count; i++) {
		int ret = 0;
		switch (req->type) {
		case LKL_DEV_BLK_TYPE_READ:
			ret = blk_pread(ctx->blk, offset, req->buf[i].iov_len,
					req->buf[i].iov_base, 0);
			break;
		case LKL_DEV_BLK_TYPE_WRITE:
			ret = blk_pwrite(ctx->blk, offset, req->buf[i].iov_len,
					 req->buf[i].iov_base, 0);
			break;
		case LKL_DEV_BLK_TYPE_FLUSH:
			ret = blk_flush(ctx->blk);
			break;
		default:
			return LKL_DEV_BLK_STATUS_IOERR;
		}
		if (ret < 0)
			return LKL_DEV_BLK_STATUS_IOERR;
		offset += req->buf[i].iov_len;
	}
	return LKL_DEV_BLK_STATUS_OK;
}

struct lkl_dev_blk_ops qemu_blk_ops = {
    .get_capacity = qemu_get_capacity,
    .request = qemu_request,
};

static int qemu_initialized = 0;

int qemu_blk_open(const char* image_path, int readonly,
		  struct lkl_disk* disk_out)
{
	Error* errp = NULL;

	if (!qemu_initialized) {
		bdrv_init();
		if (qemu_init_main_loop(&errp) < 0) {
			fprintf(stderr, "qemu_init_main_loop failed: %s\n",
				error_get_pretty(errp));
			error_free(errp);
			return -1;
		}
		qemu_initialized = 1;
	}

	int flags = readonly ? (BDRV_O_RDWR | BDRV_O_SNAPSHOT) : BDRV_O_RDWR;
	BlockBackend* blk = blk_new_open(image_path, NULL, NULL, flags, &errp);
	if (!blk) {
		fprintf(stderr, "blk_new_open(%s) failed: %s\n", image_path,
			error_get_pretty(errp));
		error_free(errp);
		return -1;
	}

	struct qemu_blk_ctx* ctx = calloc(1, sizeof(*ctx));
	ctx->blk = blk;
	ctx->capacity = blk_getlength(blk);
	if (ctx->capacity < 0) {
		fprintf(stderr, "blk_getlength failed\n");
		blk_unref(blk);
		free(ctx);
		return -1;
	}

	disk_out->handle = ctx;
	disk_out->ops = &qemu_blk_ops;
	return 0;
}

void qemu_blk_close(struct lkl_disk* disk)
{
	struct qemu_blk_ctx* ctx = disk->handle;
	if (ctx) {
		blk_unref(ctx->blk);
		free(ctx);
		disk->handle = NULL;
	}
}

const struct anyfs_backend_ops qemu_backend_ops = {
    .name = "qemu",
    .open = qemu_blk_open,
    .close = qemu_blk_close,
};
