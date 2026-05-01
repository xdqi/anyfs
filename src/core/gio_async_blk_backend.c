/*
 * GIO async block backend.
 *
 * Architecture:
 *   - A dedicated I/O thread runs a GMainLoop (owns a private GMainContext).
 *   - LKL kernel threads call request() which:
 *     1. Packages the request into a struct
 *     2. Attaches a GSource (idle) to the I/O thread's context
 *     3. Blocks on a GMutex+GCond until the async completion fires
 *   - The I/O thread processes the source, calls g_input_stream_read_async()
 *   - On completion callback (same I/O thread), signals the waiting LKL thread.
 *
 * This proves the async bridge pattern needed for Phase 3 (QEMU).
 */
#include "gio_async_blk_backend.h"

#include <gio/gio.h>
#include <lkl.h>
#include <string.h>

/* Per-request state (allocated per I/O operation) */
typedef struct {
	/* Input */
	goffset offset;
	void* buffer;
	gsize length;

	/* Completion synchronization */
	GMutex mutex;
	GCond cond;
	gboolean done;

	/* Output */
	gssize bytes_read;
	GError* error;
} AsyncReqState;

/* Per-iov chunk tracking for multi-iov requests */
typedef struct {
	AsyncReqState* state;
	GInputStream* stream;
	GSeekable* seekable;
	GMutex* stream_lock; /* protect seek+read_async atomicity */
} AsyncChunkCtx;

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

/* ---- Async read completion callback (runs on I/O thread) ---- */

static void read_complete_cb(GObject* source, GAsyncResult* res,
			     gpointer user_data)
{
	AsyncReqState* state = user_data;
	GInputStream* stream = G_INPUT_STREAM(source);

	state->bytes_read =
	    g_input_stream_read_finish(stream, res, &state->error);

	/* Signal the waiting LKL thread */
	g_mutex_lock(&state->mutex);
	state->done = TRUE;
	g_cond_signal(&state->cond);
	g_mutex_unlock(&state->mutex);
}

/* ---- Idle source callback: dispatches async read (runs on I/O thread) ---- */

typedef struct {
	struct gio_async_blk_ctx* ctx;
	AsyncReqState* state;
} IdleData;

static gboolean dispatch_read_idle(gpointer user_data)
{
	IdleData* id = user_data;
	struct gio_async_blk_ctx* ctx = id->ctx;
	AsyncReqState* state = id->state;

	/* Seek + start async read (under lock for seek atomicity) */
	g_mutex_lock(&ctx->stream_lock);

	GError* seek_err = NULL;
	if (!g_seekable_seek(G_SEEKABLE(ctx->stream), state->offset, G_SEEK_SET,
			     NULL, &seek_err)) {
		/* Seek failed — complete immediately with error */
		g_mutex_lock(&state->mutex);
		state->error = seek_err;
		state->bytes_read = -1;
		state->done = TRUE;
		g_cond_signal(&state->cond);
		g_mutex_unlock(&state->mutex);
		g_mutex_unlock(&ctx->stream_lock);
		g_free(id);
		return G_SOURCE_REMOVE;
	}

	/* Dispatch async read — completion will fire on this same context */
	g_input_stream_read_async(G_INPUT_STREAM(ctx->stream), state->buffer,
				  state->length, G_PRIORITY_HIGH,
				  NULL, /* no cancellable */
				  read_complete_cb, state);

	g_mutex_unlock(&ctx->stream_lock);
	g_free(id);
	return G_SOURCE_REMOVE;
}

/* ---- Submit one async read and wait for completion ---- */

static int submit_and_wait(struct gio_async_blk_ctx* ctx, goffset offset,
			   void* buf, gsize len)
{
	AsyncReqState state = {
	    .offset = offset,
	    .buffer = buf,
	    .length = len,
	    .done = FALSE,
	    .bytes_read = 0,
	    .error = NULL,
	};
	g_mutex_init(&state.mutex);
	g_cond_init(&state.cond);

	/* Package the request and attach to I/O thread's context */
	IdleData* id = g_new(IdleData, 1);
	id->ctx = ctx;
	id->state = &state;

	GSource* source = g_idle_source_new();
	g_source_set_callback(source, dispatch_read_idle, id, NULL);
	g_source_set_priority(source, G_PRIORITY_HIGH);
	g_source_attach(source, ctx->io_context);
	g_source_unref(source);

	/* Block until I/O thread completes the read */
	g_mutex_lock(&state.mutex);
	while (!state.done)
		g_cond_wait(&state.cond, &state.mutex);
	g_mutex_unlock(&state.mutex);

	g_mutex_clear(&state.mutex);
	g_cond_clear(&state.cond);

	if (state.error) {
		g_warning("gio_async read @%ld: %s", (long)offset,
			  state.error->message);
		g_error_free(state.error);
		return -1;
	}
	if (state.bytes_read < (gssize)len) {
		/* Short read — acceptable for end-of-file, but treat as error
		 * for block */
		return -1;
	}
	return 0;
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
	goffset offset = (goffset)req->sector * 512;

	if (req->type == LKL_DEV_BLK_TYPE_FLUSH ||
	    req->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
		return LKL_DEV_BLK_STATUS_OK;
	}

	if (req->type == LKL_DEV_BLK_TYPE_WRITE) {
		return LKL_DEV_BLK_STATUS_IOERR; /* read-only */
	}

	/* LKL_DEV_BLK_TYPE_READ: submit each iov chunk asynchronously */
	for (int i = 0; i < req->count; i++) {
		int ret = submit_and_wait(ctx, offset, req->buf[i].iov_base,
					  req->buf[i].iov_len);
		if (ret < 0)
			return LKL_DEV_BLK_STATUS_IOERR;
		offset += (goffset)req->buf[i].iov_len;
	}
	return LKL_DEV_BLK_STATUS_OK;
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
