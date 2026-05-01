#ifndef AIO_BLK_BACKEND_H
#define AIO_BLK_BACKEND_H

#include <lkl_host.h>

/**
 * aio_blk_ops - LKL block device ops using Linux native AIO + epoll.
 *
 * The request() callback returns LKL_DEV_BLK_STATUS_PENDING for READ/WRITE.
 * A dedicated I/O thread uses epoll to harvest completions and calls
 * lkl_disk_complete_req() to resume the LKL virtio path.
 *
 * Usage:
 *   struct lkl_disk disk = { .fd = open(path, O_RDONLY | O_DIRECT),
 *                            .ops = &aio_blk_ops };
 *   lkl_disk_add(&disk);
 *
 * Call aio_blk_teardown() before lkl_disk_remove() to stop the I/O thread.
 */
extern struct lkl_dev_blk_ops aio_blk_ops;

/**
 * aio_blk_teardown - stop the I/O thread and release AIO resources.
 *
 * Must be called before closing the fd / removing the disk.
 */
void aio_blk_teardown(void);

#endif /* AIO_BLK_BACKEND_H */
