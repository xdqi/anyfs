#ifndef QEMU_BACKEND_H
#define QEMU_BACKEND_H

#include <lkl_host.h>

/*
 * QEMU block backend: supports qcow2, vmdk, vdi, vhdx, vpc, etc.
 * Uses QEMU's libblock.a (statically linked) with blk_pread/blk_pwrite.
 */
#include "anyfs_backend.h"

int qemu_blk_open(const char* image_path, uint32_t flags,
		  struct lkl_disk* disk_out);
void qemu_blk_close(struct lkl_disk* disk);

/* Stop the QEMU thread (dedicated-thread embedding; no-op otherwise). Call
 * after every QEMU-backed disk has been removed and LKL has halted. */
void qemu_backend_shutdown(void);

extern struct lkl_dev_blk_ops qemu_blk_ops;

extern const struct anyfs_backend_ops qemu_backend_ops;

#endif /* QEMU_BACKEND_H */
