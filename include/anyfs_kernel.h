/*
 * anyfs_kernel.h — LKL kernel lifecycle and error reporting.
 */
#ifndef ANYFS_KERNEL_H
#define ANYFS_KERNEL_H

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

/* ── Error reporting ──────────────────────────────────────────── */

/* Backends call anyfs_set_last_error() before returning failure;
 * callers can retrieve the descriptive message. */
void anyfs_set_last_error(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
const char* anyfs_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_KERNEL_H */
