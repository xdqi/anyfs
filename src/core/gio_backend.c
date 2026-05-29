/*
 * GIO block backend: uses GFileInputStream + GSeekable for disk I/O.
 * Cross-platform alternative to raw pread/pwrite.
 *
 * Phase 2a: synchronous — g_seekable_seek() + g_input_stream_read() in
 * request(). Synchronous GIO block backend using GInputStream.
 */
#include "gio_backend.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <lkl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

struct gio_blk_ctx {
	GFile* file;
	GFileInputStream* stream; /* implements GSeekable + GInputStream */
	GMutex lock;		  /* protects seek+read atomicity */
	uint64_t capacity;
};

static int gio_get_capacity(struct lkl_disk disk, unsigned long long* res)
{
	struct gio_blk_ctx* ctx = disk.handle;
	*res = ctx->capacity;
	return 0;
}

static int gio_request(struct lkl_disk disk, struct lkl_blk_req* req)
{
	struct gio_blk_ctx* ctx = disk.handle;
	goffset offset = (goffset)req->sector * 512;
	GError* error = NULL;

	if (req->type == LKL_DEV_BLK_TYPE_FLUSH ||
	    req->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
		/* GFileInputStream is read-only, flush is no-op */
		return LKL_DEV_BLK_STATUS_OK;
	}

	if (req->type == LKL_DEV_BLK_TYPE_WRITE) {
		/* Read-only backend for now */
		return LKL_DEV_BLK_STATUS_IOERR;
	}

	/* LKL_DEV_BLK_TYPE_READ */
	g_mutex_lock(&ctx->lock);

	for (int i = 0; i < req->count; i++) {
		/* Seek to offset */
		if (!g_seekable_seek(G_SEEKABLE(ctx->stream), offset,
				     G_SEEK_SET, NULL, &error)) {
			g_warning("gio_blk seek failed: %s", error->message);
			g_error_free(error);
			g_mutex_unlock(&ctx->lock);
			return LKL_DEV_BLK_STATUS_IOERR;
		}

		/* Read data */
		gsize bytes_read = 0;
		gsize to_read = req->buf[i].iov_len;
		guint8* buf = req->buf[i].iov_base;

		if (!g_input_stream_read_all(G_INPUT_STREAM(ctx->stream), buf,
					     to_read, &bytes_read, NULL,
					     &error)) {
			g_warning("gio_blk read failed: %s", error->message);
			g_error_free(error);
			g_mutex_unlock(&ctx->lock);
			return LKL_DEV_BLK_STATUS_IOERR;
		}

		offset += (goffset)to_read;
	}

	g_mutex_unlock(&ctx->lock);
	return LKL_DEV_BLK_STATUS_OK;
}

static struct lkl_dev_blk_ops gio_ops = {
    .get_capacity = gio_get_capacity,
    .request = gio_request,
};

int gio_blk_open(const char* path, int readonly, struct lkl_disk* disk_out)
{
	(void)readonly; /* GIO backend is read-only for now */
	GError* error = NULL;

	GFile* file = g_file_new_for_path(path);
	GFileInfo* info =
	    g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
			      G_FILE_QUERY_INFO_NONE, NULL, &error);
	if (!info) {
		g_warning("gio_blk: cannot stat %s: %s", path, error->message);
		g_error_free(error);
		g_object_unref(file);
		return -1;
	}

	uint64_t capacity = (uint64_t)g_file_info_get_size(info);
	g_object_unref(info);

	/* For block devices, g_file_info_get_size returns 0 */
	if (capacity == 0) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			ioctl(fd, BLKGETSIZE64, &capacity);
			close(fd);
		}
	}

	GFileInputStream* stream = g_file_read(file, NULL, &error);
	if (!stream) {
		g_warning("gio_blk: cannot open %s: %s", path, error->message);
		g_error_free(error);
		g_object_unref(file);
		return -1;
	}

	struct gio_blk_ctx* ctx = g_new0(struct gio_blk_ctx, 1);
	g_mutex_init(&ctx->lock);
	ctx->file = file;
	ctx->stream = stream;
	ctx->capacity = capacity;

	memset(disk_out, 0, sizeof(*disk_out));
	disk_out->handle = ctx;
	disk_out->ops = &gio_ops;
	return 0;
}

void gio_blk_destroy(struct lkl_disk* disk)
{
	struct gio_blk_ctx* ctx = disk->handle;
	if (!ctx)
		return;

	g_input_stream_close(G_INPUT_STREAM(ctx->stream), NULL, NULL);
	g_object_unref(ctx->stream);
	g_object_unref(ctx->file);
	g_mutex_clear(&ctx->lock);
	g_free(ctx);
	disk->handle = NULL;
}

const struct anyfs_backend_ops gio_backend_ops = {
    .name = "gio",
    .open = gio_blk_open,
    .close = gio_blk_destroy,
};
