/*
 * GIO async block backend (true async via LKL_DEV_BLK_STATUS_PENDING).
 *
 * Architecture:
 *   - A dedicated I/O thread runs a GMainLoop (owns a private GMainContext).
 *   - LKL kernel threads call request() which:
 *     1. Packages the request (saving opaque + status_ptr)
 *     2. Attaches an idle GSource to the I/O thread's context
 *     3. Returns LKL_DEV_BLK_STATUS_PENDING immediately
 *   - The I/O thread's idle callback performs synchronous GIO reads
 *     (serialized via stream_lock), then calls lkl_disk_complete_req()
 *     to deliver the IRQ and wake the waiting LKL kernel thread.
 *
 * This is a true async backend — the LKL thread is freed during I/O.
 */
#include "gio_async_blk_backend.h"

#include <gio/gio.h>
#include <lkl.h>
#include <string.h>

/* Per-request state (heap-allocated, freed in completion) */
typedef struct {
	goffset offset;
	struct iovec* bufs;
	int buf_count;

	void* virtio_opaque; /* from lkl_blk_req.opaque */
	void* status_ptr;    /* from lkl_blk_req.status_ptr */

	struct gio_async_blk_ctx* ctx;
} AsyncReq;

struct gio_async_blk_ctx {
	GFile* file;
	GFileInputStream* stream;
	GMutex stream_lock; /* protects seek position */
	uint64_t capacity;

	/* I/O thread */
	GMainContext* io_context;
	GMainLoop* io_loop;
	GThread* io_thread;
};

/* ---- I/O Thread ---- */

static gpointer io_thread_func(gpointer data)
{
	struct gio_async_blk_ctx* ctx = data;
	g_main_context_push_thread_default(ctx->io_context);
	g_main_loop_run(ctx->io_loop);
	g_main_context_pop_thread_default(ctx->io_context);
	return NULL;
}

/* ---- Idle source callback: performs sync read on I/O thread, then completes
 * ---- */

static gboolean dispatch_read_idle(gpointer user_data)
{
	AsyncReq* ar = user_data;
	struct gio_async_blk_ctx* ctx = ar->ctx;
	uint8_t status = LKL_DEV_BLK_STATUS_OK;

	g_mutex_lock(&ctx->stream_lock);

	for (int i = 0; i < ar->buf_count; i++) {
		GError* error = NULL;

		if (!g_seekable_seek(G_SEEKABLE(ctx->stream), ar->offset,
				     G_SEEK_SET, NULL, &error)) {
			if (error)
				g_error_free(error);
			status = LKL_DEV_BLK_STATUS_IOERR;
			break;
		}

		gsize bytes_read = 0;
		if (!g_input_stream_read_all(
			G_INPUT_STREAM(ctx->stream), ar->bufs[i].iov_base,
			ar->bufs[i].iov_len, &bytes_read, NULL, &error)) {
			if (error)
				g_error_free(error);
			status = LKL_DEV_BLK_STATUS_IOERR;
			break;
		}
		if (bytes_read < ar->bufs[i].iov_len) {
			status = LKL_DEV_BLK_STATUS_IOERR;
			break;
		}

		ar->offset += ar->bufs[i].iov_len;
	}

	g_mutex_unlock(&ctx->stream_lock);

	/* Set status and complete the virtio request */
	*(uint8_t*)ar->status_ptr = status;
	lkl_disk_complete_req(ar->virtio_opaque);
	g_free(ar);

	return G_SOURCE_REMOVE;
}

/* ---- LKL block device callbacks ---- */

static int gio_async_get_capacity(struct lkl_disk disk, unsigned long long* res)
{
	struct gio_async_blk_ctx* ctx = disk.handle;
	*res = ctx->capacity;
	return 0;
}

static int gio_async_request(struct lkl_disk disk, struct lkl_blk_req* req)
{
	struct gio_async_blk_ctx* ctx = disk.handle;

	if (req->type == LKL_DEV_BLK_TYPE_FLUSH ||
	    req->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
		return LKL_DEV_BLK_STATUS_OK;
	}

	if (req->type == LKL_DEV_BLK_TYPE_WRITE) {
		return LKL_DEV_BLK_STATUS_IOERR; /* read-only */
	}

	/* Allocate async request context */
	AsyncReq* ar = g_new0(AsyncReq, 1);
	ar->offset = (goffset)req->sector * 512;
	ar->bufs = req->buf;
	ar->buf_count = req->count;
	ar->virtio_opaque = req->opaque;
	ar->status_ptr = req->status_ptr;
	ar->ctx = ctx;

	/* Dispatch to I/O thread */
	GSource* source = g_idle_source_new();
	g_source_set_callback(source, dispatch_read_idle, ar, NULL);
	g_source_set_priority(source, G_PRIORITY_HIGH);
	g_source_attach(source, ctx->io_context);
	g_source_unref(source);

	return LKL_DEV_BLK_STATUS_PENDING;
}

static struct lkl_dev_blk_ops gio_async_ops = {
    .get_capacity = gio_async_get_capacity,
    .request = gio_async_request,
};

/* ---- Open / Destroy ---- */

int gio_async_blk_open(const char* path, int readonly,
		       struct lkl_disk* disk_out)
{
	(void)readonly;
	GError* error = NULL;

	GFile* file = g_file_new_for_path(path);
	GFileInfo* info =
	    g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
			      G_FILE_QUERY_INFO_NONE, NULL, &error);
	if (!info) {
		g_warning("gio_async_blk: cannot stat %s: %s", path,
			  error->message);
		g_error_free(error);
		g_object_unref(file);
		return -1;
	}

	uint64_t capacity = (uint64_t)g_file_info_get_size(info);
	g_object_unref(info);

	GFileInputStream* stream = g_file_read(file, NULL, &error);
	if (!stream) {
		g_warning("gio_async_blk: cannot open %s: %s", path,
			  error->message);
		g_error_free(error);
		g_object_unref(file);
		return -1;
	}

	struct gio_async_blk_ctx* ctx = g_new0(struct gio_async_blk_ctx, 1);
	g_mutex_init(&ctx->stream_lock);
	ctx->file = file;
	ctx->stream = stream;
	ctx->capacity = capacity;

	/* Create private GMainContext + GMainLoop for I/O thread */
	ctx->io_context = g_main_context_new();
	ctx->io_loop = g_main_loop_new(ctx->io_context, FALSE);
	ctx->io_thread = g_thread_new("gio-async-io", io_thread_func, ctx);

	memset(disk_out, 0, sizeof(*disk_out));
	disk_out->handle = ctx;
	disk_out->ops = &gio_async_ops;
	return 0;
}

void gio_async_blk_destroy(struct lkl_disk* disk)
{
	struct gio_async_blk_ctx* ctx = disk->handle;
	if (!ctx)
		return;

	/* Stop the I/O loop and join the thread */
	g_main_loop_quit(ctx->io_loop);
	g_thread_join(ctx->io_thread);

	g_main_loop_unref(ctx->io_loop);
	g_main_context_unref(ctx->io_context);

	g_input_stream_close(G_INPUT_STREAM(ctx->stream), NULL, NULL);
	g_object_unref(ctx->stream);
	g_object_unref(ctx->file);
	g_mutex_clear(&ctx->stream_lock);
	g_free(ctx);
	disk->handle = NULL;
}
