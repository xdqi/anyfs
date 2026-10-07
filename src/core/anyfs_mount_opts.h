/*
 * anyfs_mount_opts.h — mount options anyfs adds per filesystem (internal)
 */
#ifndef ANYFS_MOUNT_OPTS_H
#define ANYFS_MOUNT_OPTS_H

#include <stddef.h>

/* A buffer of this size holds the options for every filesystem. */
#define ANYFS_MOUNT_OPTS_MAX 64

/* Write the comma-separated mount options for `fstype` into buf (cap bytes,
 * always NUL-terminated; "" when there are none). `rdonly` is non-zero for a
 * read-only mount. `fat_cp` is the codepage of FAT short names (e.g. 936),
 * or 0 for the kernel default (437). Returns 0, or -1 if the options don't
 * fit or buf is NULL or cap is 0. */
int anyfs_mount_opts(const char* fstype, int rdonly, unsigned fat_cp, char* buf,
		     size_t cap);

#endif /* ANYFS_MOUNT_OPTS_H */
