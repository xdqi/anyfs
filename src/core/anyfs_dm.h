/*
 * anyfs_dm.h — Thin wrappers over device-mapper ioctls for v2 recursion.
 *
 * Each call: create a new DM device, load a single target, resume.
 * Sectors are 512 bytes (kernel convention, independent of probed sector size).
 *
 * The caller is expected to hold the LKL kernel up and have /sys + /proc
 * mounted (done by anyfs_kernel_init). The first call lazily creates
 * /dev/mapper/control.
 *
 * On success, out_blkdev is filled with an absolute LKL path like
 * "/dev/mapper/<name>" that can be opened, used for sysfs lookup, or
 * passed back into anyfs_mount() through a synthetic mount route.
 */
#ifndef ANYFS_DM_H
#define ANYFS_DM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a dm-linear device named `name` that exposes `length_sectors`
 * (512-byte) sectors starting at `offset_sectors` of `parent_blkdev`.
 * Used for:
 *   - nested partition tables (one segment over the whole partition);
 *   - LVM linear LVs (one segment per LV extent — caller may chain).
 * Returns 0 on success and writes the new block-device path into
 * out_blkdev. Negative LKL error on failure. */
int anyfs_dm_linear(const char* parent_blkdev, uint64_t offset_sectors,
		    uint64_t length_sectors, const char* name,
		    char out_blkdev[64]);

/* Build a dm-crypt device. `cipher` is the kernel cipher spec
 * (e.g. "aes-xts-plain64"). `key` is binary, length `key_len` bytes.
 * The mapping covers `length_sectors` starting at `offset_sectors` of
 * `parent_blkdev`; IV sector numbering starts at `iv_offset`. */
int anyfs_dm_crypt(const char* parent_blkdev, uint64_t offset_sectors,
		   uint64_t length_sectors, uint64_t iv_offset,
		   const char* cipher, const uint8_t* key, size_t key_len,
		   const char* name, char out_blkdev[64]);

/* Suspend, clear, and remove the named device. Idempotent — returns 0
 * if the device was not present. */
int anyfs_dm_remove(const char* name);

/* Trigger a partition rescan on a dm device. The kernel does this
 * automatically when DM_DEV_RESUME is called for a device whose table
 * has changed, but the auto-scan races with the sysfs population we
 * want to walk afterwards. This is a synchronous BLKRRPART. */
int anyfs_dm_rescan_partitions(const char* blkdev_path);

#ifdef __cplusplus
}
#endif

#endif
