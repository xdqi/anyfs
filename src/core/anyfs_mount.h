/*
 * anyfs_mount.h — LKL mount / unmount helpers (internal)
 *
 * These are consumed only by the session layer. Public callers mount
 * via anyfs_disk_enter / anyfs_disk_enter_path.
 */
#ifndef ANYFS_MOUNT_H
#define ANYFS_MOUNT_H

#include "anyfs.h"

/* Mount a disk partition with optional auto-detection.
 * Reads /proc/filesystems and tries each type until one succeeds.
 * @disk_id   - disk id from anyfs_disk_add()
 * @part      - partition number (0 = full disk)
 * @fstype    - filesystem type (NULL or "auto" for auto-detection)
 * @name      - mount name (mounted at /lklmnt/<name>)
 * @flags     - ANYFS_MOUNT_RDONLY etc.
 * @out       - filled with mount point path and detected fstype
 * Returns 0 on success, negative on error. */
int anyfs_mount(int disk_id, unsigned int part, const char* fstype,
		const char* name, uint32_t flags, AnyfsMount* out);

/* Like anyfs_mount but takes an existing LKL-visible block-device
 * path (e.g. "/dev/mapper/foo"). Used by the session layer to mount
 * the children of a container device that doesn't have a disk_id of
 * its own (dm-linear over a partition, etc.). */
int anyfs_mount_blkdev(const char* dev_path, const char* fstype,
		       const char* name, uint32_t flags, AnyfsMount* out);

/* Unmount a previously mounted filesystem. */
int anyfs_umount(const char* name);

#endif /* ANYFS_MOUNT_H */
