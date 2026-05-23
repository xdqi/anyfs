// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_zerobench.c — upper-bound throughput probe for the host_proxy
 * + LKL TCP loopback data path.
 *
 * The producer side lives entirely inside the LKL kernel as a real
 * kthread (arch/lkl/drivers/zerobench.c, opt-in via kernel cmdline
 * lkl_zerobench_port=PORT). That kthread calls kernel_sendmsg() on a
 * 1 MiB zero buffer, so no host syscall boundary on the producer
 * path — the only futex contention on host_proxy's lkl_sys_read() is
 * the read itself, not a fight with a userspace writer.
 *
 * Data path under test:
 *   host TCP --read()-> proxy buf --lkl_sys_write()-> LKL TCP loopback
 *     --kernel--> in-kernel kthread (kernel_sendmsg, zero buffer)
 *
 * That's the ceiling we're chasing. Anything ksmbd does on top can
 * only be slower.
 *
 *   dd if=<(nc 127.0.0.1 4456) of=/dev/null bs=1M count=2000 iflag=fullblock
 */

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

#include "../host_proxy/host_proxy.h"
#include "anyfs.h"

#define LKL_ZERO_PORT 12345
#define HOST_PORT 4456

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/*
 * Bypass anyfs_kernel_init so we can pass our own kernel cmdline
 * (lkl_zerobench_port=PORT enables the in-kernel late_initcall server).
 * No disk backends are needed here.
 */
static int boot_lkl(uint16_t lkl_port, unsigned int readers)
{
	int ret = lkl_init(&lkl_host_ops);
	if (ret)
		return -1;

#ifndef _WIN32
	lkl_change_tls_mode();
#endif

	/* Memory budget can be tiny: loopback BDP is ~100 KB and the host
	 * SPSC ring is the real per-conn buffer. LKL TCP just needs enough
	 * to land a window's worth of data per socket. Env var to sweep. */
	const char* mem_env = getenv("LKL_ZEROBENCH_MEM");
	const char* mem = mem_env ? mem_env : "64M";

	char boot_args[256];
	if (readers)
		snprintf(boot_args, sizeof(boot_args),
			 "mem=%s loglevel=7 lkl_zerobench_port=%u "
			 "lkl_zerobench_readers=%u",
			 mem, (unsigned)lkl_port, readers);
	else
		snprintf(boot_args, sizeof(boot_args),
			 "mem=%s loglevel=4 lkl_zerobench_port=%u", mem,
			 (unsigned)lkl_port);
	ret = lkl_start_kernel(boot_args);
	if (ret)
		return -1;

	/* Tight TCP buffer caps. The host SPSC ring absorbs bursts; the
	 * LKL socket only needs ~1 BDP of headroom. Setting tcp_mem high
	 * relative to per-socket caps so 256 conns × 64K never trips
	 * pressure. Values in pages (4 KiB) for tcp_mem; bytes elsewhere.
	 * Set LKL_ZEROBENCH_NO_TUNE=1 to skip — measures the default. */
	if (!getenv("LKL_ZEROBENCH_NO_TUNE")) {
		(void)lkl_sysctl("net.ipv4.tcp_wmem", "4096 16384 65536");
		(void)lkl_sysctl("net.ipv4.tcp_rmem", "4096 16384 65536");
		(void)lkl_sysctl("net.ipv4.tcp_mem", "8192 16384 32768");
		(void)lkl_sysctl("net.core.wmem_default", "16384");
		(void)lkl_sysctl("net.core.rmem_default", "16384");
		(void)lkl_sysctl("net.core.wmem_max", "65536");
		(void)lkl_sysctl("net.core.rmem_max", "65536");
	}
	return 0;
}

int main(int argc, char** argv)
{
	uint16_t host_port = HOST_PORT;
	uint16_t lkl_port = LKL_ZERO_PORT;
	unsigned int readers =
	    0; /* 0 => host_proxy mode, N => in-kernel readers */
	if (argc >= 2)
		host_port = (uint16_t)atoi(argv[1]);
	if (argc >= 3)
		lkl_port = (uint16_t)atoi(argv[2]);
	if (argc >= 4)
		readers = (unsigned int)atoi(argv[3]);

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	signal(SIGPIPE, SIG_IGN);

	if (boot_lkl(lkl_port, readers) != 0) {
		fprintf(stderr, "[zerobench] LKL boot failed\n");
		return 1;
	}
	fprintf(stderr,
		"[zerobench] LKL kernel up, in-kernel zero server on :%u\n",
		(unsigned)lkl_port);

	/* lo is up after boot; the in-kernel server already bound INADDR_ANY.
	 */
	lkl_if_up(1);

	if (readers) {
		/* In-kernel readers mode: no host_proxy at all. The kthreads
		 * print their own throughput numbers via printk. */
		fprintf(stderr,
			"[zerobench] in-kernel %u-reader mode; watch dmesg\n",
			readers);
		while (g_running)
			pause();
		fprintf(stderr, "[zerobench] shutting down\n");
		lkl_sys_halt();
		lkl_cleanup();
		return 0;
	}

	if (host_proxy_start(host_port, lkl_port) < 0) {
		fprintf(stderr, "[zerobench] host_proxy_start failed\n");
		lkl_sys_halt();
		lkl_cleanup();
		return 1;
	}
	fprintf(
	    stderr,
	    "[zerobench] host proxy listening on *:%u -> LKL 127.0.0.1:%u\n",
	    (unsigned)host_port, (unsigned)lkl_port);
	fprintf(stderr,
		"[zerobench] try: dd if=<(nc 127.0.0.1 %u) of=/dev/null bs=1M "
		"count=2000 iflag=fullblock\n",
		(unsigned)host_port);

	while (g_running)
		pause();

	fprintf(stderr, "[zerobench] shutting down\n");
	host_proxy_stop();
	lkl_sys_halt();
	lkl_cleanup();
	return 0;
}
