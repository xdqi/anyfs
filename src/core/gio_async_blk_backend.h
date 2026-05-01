#ifndef GIO_ASYNC_BLK_BACKEND_H
#define GIO_ASYNC_BLK_BACKEND_H

#include <lkl_host.h>

/*
 * GIO async block backend: I/O dispatched through GMainLoop.
 *
 * A dedicated I/O thread runs a GMainLoop. LKL's request() callback
 * submits work to this loop and blocks until the async completion fires.
 * This demonstrates the Phase 3 pattern (QEMU AioContext on GMainLoop).
 *
 * For local files, GIO internally uses a thread pool for the actual read,
 * but the completion always fires on our GMainContext — proving the pattern.
 */
int gio_async_blk_open(const char* path, int readonly,
		       struct lkl_disk* disk_out);
void gio_async_blk_destroy(struct lkl_disk* disk);

#endif /* GIO_ASYNC_BLK_BACKEND_H */
