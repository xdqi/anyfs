/*
 * anyfs.h — Minimal LKL filesystem access library
 *
 * Provides kernel init/destroy and multi-backend disk registration.
 * After adding a disk, use LKL syscall APIs directly (lkl_sys_open, etc.)
 */
#ifndef ANYFS_H
#define ANYFS_H

#include <lkl.h>
#include <lkl_host.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Kernel lifecycle ─────────────────────────────────────────── */

typedef struct {
	uint32_t mem_mb;   /* LKL kernel memory in MB (default: 64) */
	uint32_t loglevel; /* Kernel log level 0-7 (default: 0 = silent) */
} AnyfsKernelOpts;

/* Start the LKL kernel. Call once before any disk operations.
 * Pass NULL for defaults. Returns 0 on success, negative on error. */
int anyfs_kernel_init(const AnyfsKernelOpts* opts);

/* Halt the LKL kernel. All disks must be removed first. */
void anyfs_kernel_halt(void);

/* ── Disk management ──────────────────────────────────────────── */

#define ANYFS_DISK_READONLY (1u << 0)
#define ANYFS_BACKEND_RAW (1u << 1)
#define ANYFS_BACKEND_GIO (1u << 2)
#define ANYFS_BACKEND_QEMU (1u << 3)

/* Add a disk image. Selects backend based on flags (or auto-detect if none).
 * Returns LKL disk_id >= 0 on success, negative on error.
 * After this, use lkl_mount_dev(disk_id, ...) or anyfs_mount(). */
int anyfs_disk_add(const char* image_path, uint32_t flags);

/* Remove a previously added disk. Unmount first. */
int anyfs_disk_remove(int disk_id);

/* ── Mount management ─────────────────────────────────────────── */

#define ANYFS_MOUNT_RDONLY (1u << 0)

typedef struct {
	char mount_point[64]; /* Filled: actual mount path (/lklmnt/<name>) */
	char fstype[32];      /* Filled: detected filesystem type */
} AnyfsMount;

/* Mount a disk (partition) with automatic filesystem detection.
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

/* Unmount a previously mounted filesystem. */
int anyfs_umount(const char* name);

/* Remount an existing mount as read-only. */
int anyfs_remount_ro(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_H */
