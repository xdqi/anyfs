/*
 * server_common.h — Shared daemon skeleton for the LKL server surfaces
 * (anyfs-ksmbd, anyfs-nfsd): stop flag + signal install, kernel boot with
 * loopback up, the --share resolution loop, and shutdown. Arg parsing and
 * the serving loops stay in the respective mains.
 */
#ifndef ANYFS_SERVER_COMMON_H
#define ANYFS_SERVER_COMMON_H

#include "anyfs.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while serving; cleared by SIGINT/SIGTERM. Serving loops poll it. */
extern volatile sig_atomic_t anyfs_server_running;

/* Unbuffer stdout and route SIGINT/SIGTERM to clearing
 * anyfs_server_running. */
void anyfs_server_install_signals(void);

/* Boot the LKL kernel and bring up loopback (ifindex 1, idempotent —
 * the in-kernel listeners bind to it). Returns anyfs_kernel_init()'s
 * result: 0 on success. */
int anyfs_server_boot(const AnyfsKernelOpts* opts);

typedef struct AnyfsShareEntry {
	char name[64];
	char lkl_path[ANYFS_LKL_PATH_MAX];
} AnyfsShareEntry;

/* Resolve every --share spec to a (name, lkl_path) pair via
 * anyfs_share_resolve(), which prints its own diagnostics. Returns the
 * number of entries filled, or -1 on the first failure. */
int anyfs_server_resolve_shares(char* const* specs, int n_specs,
				AnyfsSession** disks, int n_disks,
				uint32_t enter_flags, AnyfsShareEntry* out,
				int max_out);

/* Close every non-NULL session and halt the kernel. */
void anyfs_server_shutdown(AnyfsSession** disks, int n_disks);

#ifdef __cplusplus
}
#endif

#endif
