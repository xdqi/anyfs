#ifndef RAW_BACKEND_H
#define RAW_BACKEND_H

#include <lkl_host.h>

/*
 * Create a raw block backend using pread/pwrite on a file descriptor.
 * @flags is the same bitmask as anyfs_disk_add (ANYFS_SESSION_*).
 * The returned lkl_disk is ready for lkl_disk_add().
 * Caller must eventually call raw_blk_destroy() to free resources.
 */
#include "anyfs_backend.h"

int raw_blk_open(const char* path, uint32_t flags, struct lkl_disk* disk_out);
void raw_blk_destroy(struct lkl_disk* disk);

extern const struct anyfs_backend_ops raw_backend_ops;

#endif /* RAW_BACKEND_H */
