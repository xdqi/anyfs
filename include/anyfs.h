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

/* ── Shared flags ──────────────────────────────────────────────── */

/* Flags for anyfs_disk_open / anyfs_session_open:
 *   ANYFS_DISK_READONLY  — open disk read-only
 *   ANYFS_BACKEND_RAW    — force raw (pread/pwrite) backend
 *   ANYFS_BACKEND_GIO    — force GIO backend
 *   ANYFS_BACKEND_QEMU   — force QEMU block backend
 *   ANYFS_MOUNT_RDONLY   — mount filesystem read-only (used by
 *                           anyfs_disk_enter / anyfs_disk_enter_path)
 */
#define ANYFS_DISK_READONLY (1u << 0)
#define ANYFS_BACKEND_RAW (1u << 1)
#define ANYFS_BACKEND_GIO (1u << 2)
#define ANYFS_BACKEND_QEMU (1u << 3)
#define ANYFS_MOUNT_RDONLY (1u << 0)

/* ── Mount info (returned by internal mount helpers) ──────────── */

typedef struct {
	char mount_point[64]; /* Filled: actual mount path (/lklmnt/<name>) */
	char fstype[32];      /* Filled: detected filesystem type */
} AnyfsMount;

/* ── Error reporting ──────────────────────────────────────────── */

/* Backends call anyfs_set_last_error() before returning failure;
 * callers can retrieve the descriptive message. */
void anyfs_set_last_error(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
const char* anyfs_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_H */
