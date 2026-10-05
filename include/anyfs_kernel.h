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

/* ── Fatal errors ─────────────────────────────────────────────── */

/* A fatal error means the engine is wedged (e.g. the QEMU thread missed its
 * watchdog deadline) and the process must not be trusted any further. The
 * reporting thread does not return to its caller afterwards; it may block
 * forever. Each embedder installs a hook that tears the engine down from the
 * outside: the wasm glue tells the page to terminate the worker, the native
 * addon tells JS. Without a hook (CLI servers) the reason is printed to
 * stderr and the process exits with status 1. The hook runs at most once,
 * on an arbitrary thread. */
typedef void (*anyfs_fatal_hook_fn)(const char* reason);
void anyfs_set_fatal_hook(anyfs_fatal_hook_fn fn);
void anyfs_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_KERNEL_H */
