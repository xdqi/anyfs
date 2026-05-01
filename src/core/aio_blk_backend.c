/*
 * aio_blk_backend.c - True async block backend using Linux AIO + eventfd.
 *
 * Architecture (threadless):
 *   - LKL's request() returns LKL_DEV_BLK_STATUS_PENDING immediately
 *   - I/O is submitted via io_submit() (kernel AIO) with IOCB_FLAG_RESFD
 *   - The eventfd is registered with lkl_host_ops.idle_add_event()
 *   - LKL's idle loop polls the eventfd and calls our handler to reap
 *   - Completions call lkl_disk_complete_req() directly from idle context
 *
 * NO separate reaper thread — completions fire in LKL's idle path.
 *
 * Requirements:
 *   - O_DIRECT fd (AIO requires it for truly async behavior)
 *   - Linux >= 4.18 (for eventfd-based AIO notification)
 *   - LKL host_ops.idle + idle_add_event must be provided
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "aio_blk_backend.h"
#include <lkl.h>
#include <lkl_host.h>

/* Linux AIO syscall wrappers (glibc doesn't wrap these) */
static inline int io_setup(unsigned nr, aio_context_t* ctxp)
{
	return syscall(__NR_io_setup, nr, ctxp);
}

static inline int io_destroy(aio_context_t ctx)
{
	return syscall(__NR_io_destroy, ctx);
}

static inline int io_submit(aio_context_t ctx, long nr, struct iocb** iocbpp)
{
	return syscall(__NR_io_submit, ctx, nr, iocbpp);
}

static inline int io_getevents(aio_context_t ctx, long min_nr, long max_nr,
			       struct io_event* events,
			       struct timespec* timeout)
{
	return syscall(__NR_io_getevents, ctx, min_nr, max_nr, events, timeout);
}

/* Max in-flight I/O requests */
#define AIO_MAX_INFLIGHT 256
#define AIO_REAP_BATCH 64

/* Per-request context saved alongside the iocb */
struct aio_req {
	struct iocb iocb;
	void* virtio_opaque; /* lkl_blk_req.opaque (virtio_req *) */
	void* status_ptr;    /* lkl_blk_req.status_ptr → trailer byte */
	void* bounce_buf;    /* aligned buffer for O_DIRECT */
	void* user_buf;	     /* original unaligned destination */
	size_t user_len;     /* bytes to copy back on read completion */
	int is_read;
};

/* Global state (single AIO context for all disks) */
static struct {
	aio_context_t aio_ctx;
	int event_fd;
	int initialized;
	/* Simple pool of aio_req to avoid malloc per I/O */
	struct aio_req pool[AIO_MAX_INFLIGHT];
	volatile int pool_used[AIO_MAX_INFLIGHT]; /* 0=free, 1=in-use */
} g_aio;

static struct aio_req* alloc_aio_req(void)
{
	for (int i = 0; i < AIO_MAX_INFLIGHT; i++) {
		if (!g_aio.pool_used[i]) {
			g_aio.pool_used[i] = 1;
			memset(&g_aio.pool[i], 0, sizeof(struct aio_req));
			return &g_aio.pool[i];
		}
	}
	return NULL; /* all slots busy */
}

static void free_aio_req(struct aio_req* r)
{
	if (r->bounce_buf) {
		free(r->bounce_buf);
		r->bounce_buf = NULL;
	}
	int idx = (int)(r - g_aio.pool);
	g_aio.pool_used[idx] = 0;
}

/*
 * Idle event handler: called from LKL's idle loop when eventfd is readable.
 * Reaps completed AIO requests and calls lkl_disk_complete_req().
 */
static void aio_idle_handler(void* arg)
{
	(void)arg;
	struct io_event events[AIO_REAP_BATCH];

	/* Consume the eventfd counter */
	uint64_t val;
	if (read(g_aio.event_fd, &val, sizeof(val)) < 0)
		return;

	/* Reap completed I/Os (non-blocking) */
	struct timespec ts = {0, 0};
	int n = io_getevents(g_aio.aio_ctx, 1, AIO_REAP_BATCH, events, &ts);

	for (int i = 0; i < n; i++) {
		struct aio_req* r = (struct aio_req*)events[i].obj;
		long res = (long)events[i].res;

		/* Set status in virtio trailer */
		uint8_t* status = (uint8_t*)r->status_ptr;
		if (res >= 0) {
			*status = LKL_DEV_BLK_STATUS_OK;
			/* Copy from bounce buffer to user buffer on read */
			if (r->is_read && r->bounce_buf && r->user_buf)
				memcpy(r->user_buf, r->bounce_buf, r->user_len);
		} else {
			*status = LKL_DEV_BLK_STATUS_IOERR;
		}

		/* Complete the virtio request (wakes LKL thread) */
		lkl_disk_complete_req(r->virtio_opaque);
		free_aio_req(r);
	}
}

static int aio_init_once(void)
{
	static int initialized;

	if (initialized)
		return 0;

	memset(&g_aio, 0, sizeof(g_aio));

	/* Create AIO context */
	if (io_setup(AIO_MAX_INFLIGHT, &g_aio.aio_ctx) < 0) {
		perror("io_setup");
		return -1;
	}

	/* Create eventfd for AIO completion notification */
	g_aio.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (g_aio.event_fd < 0) {
		perror("eventfd");
		io_destroy(g_aio.aio_ctx);
		return -1;
	}

	/* Register eventfd with LKL's idle loop */
	if (lkl_host_ops.idle_add_event) {
		lkl_host_ops.idle_add_event((void*)(intptr_t)g_aio.event_fd,
					    aio_idle_handler, NULL);
	} else {
		fprintf(stderr, "aio_blk: WARNING: no idle_add_event, "
				"completions may not fire\n");
	}

	initialized = 1;
	g_aio.initialized = 1;
	return 0;
}

void aio_blk_teardown(void)
{
	if (!g_aio.initialized)
		return;

	/* Unregister from idle loop */
	if (lkl_host_ops.idle_del_event)
		lkl_host_ops.idle_del_event((void*)(intptr_t)g_aio.event_fd);

	close(g_aio.event_fd);
	io_destroy(g_aio.aio_ctx);
	g_aio.initialized = 0;
}

static int aio_get_capacity(struct lkl_disk disk, unsigned long long* res)
{
	off_t sz = lseek(disk.fd, 0, SEEK_END);
	if (sz < 0)
		return -1;
	*res = (unsigned long long)sz;
	return 0;
}

/*
 * Allocate a 512-byte aligned bounce buffer for O_DIRECT.
 * Linux AIO with O_DIRECT requires aligned buffers.
 */
static void* alloc_aligned(size_t len)
{
	void* ptr = NULL;
	if (posix_memalign(&ptr, 512, len) != 0)
		return NULL;
	return ptr;
}

static int aio_request(struct lkl_disk disk, struct lkl_blk_req* req)
{
	if (aio_init_once() < 0)
		return LKL_DEV_BLK_STATUS_IOERR;

	if (req->type == LKL_DEV_BLK_TYPE_FLUSH ||
	    req->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
		/* Flush is synchronous (fdatasync), then complete inline.
		 * Returning OK means virtio_blk.c will call virtio_req_complete
		 * synchronously. */
		if (fdatasync(disk.fd) < 0)
			return LKL_DEV_BLK_STATUS_IOERR;
		return LKL_DEV_BLK_STATUS_OK;
	}

	/* For multi-segment I/O, we merge into a single bounce buffer.
	 * This keeps the AIO submission simple (one iocb per request). */
	size_t total_len = 0;
	for (int i = 0; i < req->count; i++)
		total_len += req->buf[i].iov_len;

	int64_t offset = (int64_t)req->sector * 512;

	struct aio_req* ar = alloc_aio_req();
	if (!ar)
		return LKL_DEV_BLK_STATUS_IOERR;

	ar->virtio_opaque = req->opaque;
	ar->status_ptr = req->status_ptr;

	/* Allocate aligned bounce buffer */
	ar->bounce_buf = alloc_aligned(total_len);
	if (!ar->bounce_buf) {
		free_aio_req(ar);
		return LKL_DEV_BLK_STATUS_IOERR;
	}

	if (req->type == LKL_DEV_BLK_TYPE_READ) {
		ar->is_read = 1;
		ar->user_buf = req->buf[0].iov_base;
		ar->user_len = total_len;
	} else {
		/* Write: copy data into bounce buffer */
		ar->is_read = 0;
		char* p = ar->bounce_buf;
		for (int i = 0; i < req->count; i++) {
			memcpy(p, req->buf[i].iov_base, req->buf[i].iov_len);
			p += req->buf[i].iov_len;
		}
	}

	/* Prepare iocb */
	struct iocb* cb = &ar->iocb;
	memset(cb, 0, sizeof(*cb));
	cb->aio_fildes = disk.fd;
	cb->aio_lio_opcode = (req->type == LKL_DEV_BLK_TYPE_READ)
				 ? IOCB_CMD_PREAD
				 : IOCB_CMD_PWRITE;
	cb->aio_buf = (uint64_t)(uintptr_t)ar->bounce_buf;
	cb->aio_nbytes = total_len;
	cb->aio_offset = offset;
	/* Set eventfd for completion notification */
	cb->aio_flags = IOCB_FLAG_RESFD;
	cb->aio_resfd = g_aio.event_fd;

	struct iocb* cbs[1] = {cb};
	int ret = io_submit(g_aio.aio_ctx, 1, cbs);
	if (ret != 1) {
		/* io_submit failed — fall back to sync */
		if (req->type == LKL_DEV_BLK_TYPE_READ) {
			ssize_t rd =
			    pread(disk.fd, ar->bounce_buf, total_len, offset);
			if (rd >= 0)
				memcpy(ar->user_buf, ar->bounce_buf, total_len);
			free_aio_req(ar);
			return (rd >= 0) ? LKL_DEV_BLK_STATUS_OK
					 : LKL_DEV_BLK_STATUS_IOERR;
		} else {
			ssize_t wr =
			    pwrite(disk.fd, ar->bounce_buf, total_len, offset);
			free_aio_req(ar);
			return (wr >= 0) ? LKL_DEV_BLK_STATUS_OK
					 : LKL_DEV_BLK_STATUS_IOERR;
		}
	}

	return LKL_DEV_BLK_STATUS_PENDING;
}

struct lkl_dev_blk_ops aio_blk_ops = {
    .get_capacity = aio_get_capacity,
    .request = aio_request,
};
