#ifndef GIO_BLK_BACKEND_H
#define GIO_BLK_BACKEND_H

#include <lkl_host.h>

/*
 * GIO-based block backend using GFileInputStream + GSeekable.
 * Cross-platform: works on Linux/Windows/macOS via GIO abstraction.
 * Phase 2a: synchronous (seek + read in request callback).
 */
#include "anyfs_backend.h"

int gio_blk_open(const char* path, int readonly, struct lkl_disk* disk_out);
void gio_blk_destroy(struct lkl_disk* disk);

extern const struct anyfs_backend_ops gio_backend_ops;

#endif /* GIO_BLK_BACKEND_H */
