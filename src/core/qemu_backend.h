#ifndef QEMU_BACKEND_H
#define QEMU_BACKEND_H

#include <lkl_host.h>

/*
 * QEMU block backend: supports qcow2, vmdk, vdi, vhdx, vpc, etc.
 * Uses QEMU's libblock.a (statically linked) with blk_pread/blk_pwrite.
 */
#include "anyfs_backend.h"

int qemu_blk_open(const char* image_path, int readonly,
		  struct lkl_disk* disk_out);
void qemu_blk_close(struct lkl_disk* disk);

extern const struct anyfs_backend_ops qemu_backend_ops;

#endif /* QEMU_BACKEND_H */
