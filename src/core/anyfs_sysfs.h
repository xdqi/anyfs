/*
 * anyfs_sysfs.h — Read partition geometry from LKL's /sys/block/<vdN>/.
 */
#ifndef ANYFS_SYSFS_H
#define ANYFS_SYSFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned int index;   /* 1-based partition table slot */
	uint64_t start_bytes; /* offset of partition on disk */
	uint64_t size_bytes;
	int read_only;
	char name[64]; /* basename, e.g. "vda1" */
} AnyfsSysfsPart;

/* Resolve the kernel-visible block name (e.g. "vda") for a given
 * disk_id via /sys/dev/block/<major>:<minor>. `dev` is the encoded
 * dev_t from lkl_get_virtio_blkdev(disk_id, 0, ...). Returns 0 on
 * success, negative on error. */
int anyfs_sysfs_resolve_disk_name(uint32_t dev, char out[64]);

/* Walk /sys/block/<disk_name>/ and fill the partition list. Returns
 * the number of entries written (capped at buf_n) or negative on
 * error. */
int anyfs_sysfs_walk(const char* disk_name, AnyfsSysfsPart* buf, size_t buf_n);

#ifdef __cplusplus
}
#endif

#endif
