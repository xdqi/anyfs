/*
 * anyfs_kernel.c — LKL kernel lifecycle + error reporting + atexit cleanup
 */
#define _GNU_SOURCE
#include "anyfs.h"
#include "anyfs_backend.h"
#include "raw_backend.h"
#ifdef ANYFS_HAS_GIO
#include "gio_backend.h"
#endif
#ifdef ANYFS_HAS_QEMU
#include "qemu_backend.h"
#endif
#include "anyfs_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal state ────────────────────────────────────────────── */

/* Thread-local last-error buffer, so backends can report details. */
static __thread char g_last_error[512];

void anyfs_set_last_error(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
	va_end(ap);
}

const char* anyfs_get_last_error(void)
{
	return g_last_error[0] ? g_last_error : NULL;
}

static int g_kernel_started;

/* atexit safety net: read /proc/mounts, unmount everything under /lklmnt/,
 * remove all disks, then halt the kernel. */
static void anyfs_atexit_cleanup(void)
{
	if (!g_kernel_started)
		return;

	lkl_sys_sync();

	/* Read /proc/mounts to find all our mount points */
	int fd = lkl_sys_open("/proc/mounts", LKL_O_RDONLY, 0);
	if (fd >= 0) {
		char buf[4096];
		long n;
		while ((n = lkl_sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
			buf[n] = '\0';
			/* Parse lines: "device mountpoint fstype options ..."
			 */
			char* line = buf;
			while (line && *line) {
				char* eol = strchr(line, '\n');
				if (eol)
					*eol = '\0';

				/* Find mount point (second field) */
				char* mp = strchr(line, ' ');
				if (mp) {
					mp++;
					char* mp_end = strchr(mp, ' ');
					if (mp_end)
						*mp_end = '\0';

					if (strncmp(mp, "/lklmnt/", 8) == 0) {
						lkl_sys_umount(mp, 0);
					}
				}

				line = eol ? eol + 1 : NULL;
			}
		}
		lkl_sys_close(fd);
	}

	/* Remove all disk devices */
	for (int i = 0; i < ANYFS_MAX_DISKS; i++) {
		if (g_disks[i].in_use) {
			lkl_disk_remove(g_disks[i].disk);
			g_disks[i].backend->close(&g_disks[i].disk);
			g_disks[i].in_use = 0;
		}
	}

	lkl_sys_halt();
	lkl_cleanup();
	g_kernel_started = 0;
}

/* ── Kernel lifecycle ─────────────────────────────────────────── */

int anyfs_kernel_init(const AnyfsKernelOpts* opts)
{
	if (g_kernel_started)
		return 0;

	/* 32M default: with the tight TCP sysctls applied below, the kernel
	 * needs essentially no memory for the TCP path (bench: 2M still hits
	 * 3 GB/s × 16 streams). 32M leaves headroom for VFS / FS slabs. */
	uint32_t mem_mb = 32;
	uint32_t loglevel = 0;
	if (opts) {
		if (opts->mem_mb)
			mem_mb = opts->mem_mb;
		loglevel = opts->loglevel;
	}

	/* Register backends */
	anyfs_register_backend(&raw_backend_ops);
#ifdef ANYFS_HAS_GIO
	anyfs_register_backend(&gio_backend_ops);
#endif
#ifdef ANYFS_HAS_QEMU
	anyfs_register_backend(&qemu_backend_ops);
#endif

	int ret = lkl_init(&lkl_host_ops);
	if (ret)
		return -1;

	/* Work around TLS issues when LKL is loaded as a shared library.
	 * Without this, pthread_getspecific() may fail due to duplicated
	 * __pthread_keys across namespaces (see posix-host.c).
	 * Not applicable to Windows (no RTLD_LOCAL equivalent for DLLs). */
#ifndef _WIN32
	lkl_change_tls_mode();
#endif

	char boot_args[128];
	snprintf(boot_args, sizeof(boot_args), "mem=%uM loglevel=%u", mem_mb,
		 loglevel);
	ret = lkl_start_kernel(boot_args);
	if (ret)
		return -1;

	g_kernel_started = 1;

	/* Tight TCP buffer caps. Default tcp_mem is auto-sized to available
	 * kernel memory (tiny when mem= is small), so concurrent TCP servers
	 * (ksmbd/nfsd) hit memory pressure and stall under multi-stream load
	 * unless mem= is large. Pinning tcp_mem high relative to per-socket
	 * caps decouples throughput from kernel memory size — measured:
	 * 16 streams at 3+ GB/s on mem=2M with these caps, vs 140 MB/s on
	 * mem=64M with the kernel defaults. Per-socket caps are small
	 * (~64 KiB) because the user-space SPSC ring absorbs bursts; LKL
	 * socket buffers only need ~1 loopback BDP of headroom.
	 * tcp_mem values are in pages (4 KiB); the rest are bytes. */
	(void)lkl_sysctl("net.ipv4.tcp_wmem", "4096 16384 65536");
	(void)lkl_sysctl("net.ipv4.tcp_rmem", "4096 16384 65536");
	(void)lkl_sysctl("net.ipv4.tcp_mem", "8192 16384 32768");
	(void)lkl_sysctl("net.core.wmem_default", "16384");
	(void)lkl_sysctl("net.core.rmem_default", "16384");
	(void)lkl_sysctl("net.core.wmem_max", "65536");
	(void)lkl_sysctl("net.core.rmem_max", "65536");

	/* Mount sysfs early — the multi-partition session layer walks
	 * /sys/block/<vdN>/ to discover partitions. Tolerate EEXIST/EBUSY
	 * if something already mounted it. */
	lkl_sys_mkdir("/sys", 0555);
	{
		long mret = lkl_sys_mount("sysfs", "/sys", "sysfs", 0, NULL);
		(void)mret; /* -EBUSY/-EEXIST are fine */
	}
	/* procfs is mounted lazily by anyfs_mount() when needed; the
	 * session layer also benefits from it being available — mount it
	 * here too so /proc/mounts/filesystems are usable right after
	 * kernel init. */
	lkl_sys_mkdir("/proc", 0555);
	{
		long mret = lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
		(void)mret;
	}

	atexit(anyfs_atexit_cleanup);
	return 0;
}

void anyfs_kernel_halt(void)
{
	if (!g_kernel_started)
		return;
	lkl_sys_halt();
	lkl_cleanup();
	g_kernel_started = 0;
}
