#ifndef GIO_BLK_BACKEND_H
#define GIO_BLK_BACKEND_H

#include <lkl_host.h>

/*
 * GIO-based block backend using GFileInputStream + GSeekable.
 * Cross-platform: works on Linux/Windows/macOS via GIO abstraction.
 * Phase 2a: synchronous (seek + read in request callback).
 */
int gio_blk_open(const char* path, int readonly, struct lkl_disk* disk_out);
void gio_blk_destroy(struct lkl_disk* disk);

#endif /* GIO_BLK_BACKEND_H */
