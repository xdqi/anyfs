// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_shmem_relay_bench.c — measure the per-connection bandwidth of the
 * in-LKL shmem_relay driver and compare against the lkl_sys_read path.
 *
 * Three configurations are exercised, picked via argv[1]:
 *   "relay"   - data path: in-kernel zerobench writer kthread
 *                          -> inner LKL TCP socket
 *                          -> relay kthread (kernel_recvmsg)
 *                          -> shared l2h ring
 *                          -> host consumer thread (memcpy/discard)
 *               No lkl_sys_* on the data path after attach.
 *
 *   "syscall" - data path: in-kernel zerobench writer kthread
 *                          -> inner LKL TCP socket
 *                          -> host pthread calling lkl_sys_read()
 *                          -> discard
 *               Same producer, same socket; only difference is the
 *               consumer side. This is the apples-to-apples baseline.
 *
 *   "both"    - run "syscall" first, then "relay", reporting both.
 *
 * Configure throughput target with argv[2] (MiB, default 4096).
 *
 * Memory layout: rings are plain malloc()'d buffers in this process's
 * address space. LKL kernel and host userspace share that address
 * space (LKL is a library), so SHMEM_RELAY_ATTACH just hands the
 * kernel raw pointers and atomic64 counters to dereference.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl/linux/kdev_t.h>
#include <lkl_host.h>

#define LKL_ZERO_PORT 12345
#define RING_CAP (4u * 1024u * 1024u) /* 4 MiB SPSC ring */
#define DEFAULT_TARGET_MB 4096
#define CHUNK_BYTES (1u << 20) /* host-side read chunk */

/* Mirror of struct shmem_relay_attach_arg in arch/lkl/drivers/shmem_relay.c. */
struct shmem_relay_attach_arg {
	uint32_t sock_fd;
	uint32_t ring_cap;
	uint64_t h2l_buf;
	uint64_t h2l_head;
	uint64_t h2l_tail;
	uint64_t l2h_buf;
	uint64_t l2h_head;
	uint64_t l2h_tail;
};

#define SHMEM_RELAY_IOC_MAGIC 'S'
#define SHMEM_RELAY_ATTACH                                                     \
	_IOW(SHMEM_RELAY_IOC_MAGIC, 1, struct shmem_relay_attach_arg)
#define SHMEM_RELAY_KICK _IO(SHMEM_RELAY_IOC_MAGIC, 3)

static double now_sec(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int boot_lkl(uint16_t lkl_port)
{
	int ret = lkl_init(&lkl_host_ops);
	if (ret)
		return -1;
#ifndef _WIN32
	lkl_change_tls_mode();
#endif
	char boot_args[192];
	/* mem=1024M: TCP memory pressure surfaces under sustained writes
	 * with default 64M/128M. Same gotcha as host_proxy fix. */
	snprintf(boot_args, sizeof(boot_args),
		 "mem=1024M loglevel=4 lkl_zerobench_port=%u",
		 (unsigned)lkl_port);
	if (lkl_start_kernel(boot_args))
		return -1;
	lkl_if_up(1);
	return 0;
}

static int lkl_open_connection(uint16_t lkl_port)
{
	long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
	if (s < 0) {
		fprintf(stderr, "lkl_sys_socket: %s\n", lkl_strerror((int)s));
		return -1;
	}
	struct lkl_sockaddr_in sin = {0};
	sin.sin_family = LKL_AF_INET;
	sin.sin_port = htons(lkl_port);
	sin.sin_addr.lkl_s_addr = htonl(0x7f000001);
	long r =
	    lkl_sys_connect((int)s, (struct lkl_sockaddr*)&sin, sizeof(sin));
	if (r < 0) {
		fprintf(stderr, "lkl_sys_connect: %s\n", lkl_strerror((int)r));
		lkl_sys_close((int)s);
		return -1;
	}
	return (int)s;
}

/* ---------- syscall baseline ---------- */

static double run_syscall_bench(uint16_t lkl_port, uint64_t target_bytes)
{
	int s = lkl_open_connection(lkl_port);
	if (s < 0)
		return -1.0;

	void* buf = malloc(CHUNK_BYTES);
	if (!buf) {
		lkl_sys_close(s);
		return -1.0;
	}

	uint64_t total = 0;
	double t0 = now_sec();
	while (total < target_bytes) {
		long n = lkl_sys_read(s, buf, CHUNK_BYTES);
		if (n <= 0) {
			if (n == -LKL_EINTR)
				continue;
			fprintf(stderr, "[syscall] lkl_sys_read: %ld (%s)\n", n,
				n < 0 ? lkl_strerror((int)n) : "EOF");
			break;
		}
		total += (uint64_t)n;
	}
	double t1 = now_sec();

	free(buf);
	/* Half-shut suffices: the in-kernel writer hits EPIPE on next send
	 * and exits. We don't care about clean drain here. */
	lkl_sys_shutdown(s, 2 /* SHUT_RDWR */);
	lkl_sys_close(s);

	double secs = t1 - t0;
	double mb = (double)total / (1024.0 * 1024.0);
	fprintf(stderr, "[syscall] %.0f MB in %.3f s -> %.2f MB/s\n", mb, secs,
		mb / secs);
	return mb / secs;
}

/* ---------- shmem relay path ---------- */

struct ring_meta {
	_Atomic uint64_t head; /* producer writes */
	_Atomic uint64_t tail; /* consumer writes */
};

struct relay_ctx {
	void* l2h_buf;
	struct ring_meta* l2h;
	uint32_t cap;
	uint64_t target;
	int relay_fd;
	volatile int done;
	uint64_t total;
	double elapsed;
};

static void* relay_consumer(void* arg)
{
	struct relay_ctx* r = arg;
	uint32_t cap = r->cap;
	void* buf = r->l2h_buf;
	uint64_t total = 0;
	double t0 = now_sec();

	/* Spin-consume the l2h ring. We don't need to keep the bytes —
	 * the bench is "produce + transport at line rate to userspace". */
	int kick_streak = 0;
	while (total < r->target) {
		uint64_t head =
		    atomic_load_explicit(&r->l2h->head, memory_order_acquire);
		uint64_t tail =
		    atomic_load_explicit(&r->l2h->tail, memory_order_relaxed);
		uint32_t avail = (uint32_t)(head - tail);
		if (!avail) {
			sched_yield();
			continue;
		}
		uint32_t idx = (uint32_t)tail & (cap - 1);
		uint32_t contig = cap - idx;
		uint32_t n = avail < contig ? avail : contig;
		/* Touch the bytes so the read isn't elided by an over-eager
		 * compiler — sum into a sink. */
		volatile uint8_t* p = (uint8_t*)buf + idx;
		uint8_t sink = 0;
		for (uint32_t i = 0; i < n; i += 64)
			sink ^= p[i];
		(void)sink;
		bool was_full = (head - tail) == cap;
		atomic_store_explicit(&r->l2h->tail, tail + n,
				      memory_order_release);
		total += n;
		/* Kick if recv_kt is likely blocked waiting for ring space, or
		 * every ~16 chunks just to keep things moving. The ioctl itself
		 * is the cost we're measuring; we want to minimise it. */
		if (was_full || ++kick_streak >= 16) {
			lkl_sys_ioctl(r->relay_fd, SHMEM_RELAY_KICK, 0);
			kick_streak = 0;
		}
	}

	double t1 = now_sec();
	r->total = total;
	r->elapsed = t1 - t0;
	r->done = 1;
	return NULL;
}

static double run_relay_bench(uint16_t lkl_port, uint64_t target_bytes)
{
	int sock_fd = lkl_open_connection(lkl_port);
	if (sock_fd < 0)
		return -1.0;

	/* LKL DEVTMPFS=n -> we mknod the misc node ourselves at the
	 * fixed minor exported by arch/lkl/drivers/shmem_relay.c. */
	(void)lkl_sys_mknod(
	    "/dev/lkl_shmem_relay", LKL_S_IFCHR | 0600,
	    LKL_MKDEV(10, 240) /* MISC_MAJOR, SHMEM_RELAY_MINOR */);
	int relay_fd = (int)lkl_sys_open("/dev/lkl_shmem_relay", LKL_O_RDWR, 0);
	if (relay_fd < 0) {
		fprintf(stderr, "lkl_sys_open(/dev/lkl_shmem_relay): %s\n",
			lkl_strerror(relay_fd));
		lkl_sys_close(sock_fd);
		return -1.0;
	}

	/* Allocate rings + counters in this process's address space. */
	void* h2l_buf = aligned_alloc(64, RING_CAP);
	void* l2h_buf = aligned_alloc(64, RING_CAP);
	struct ring_meta* h2l = aligned_alloc(64, sizeof(*h2l));
	struct ring_meta* l2h = aligned_alloc(64, sizeof(*l2h));
	if (!h2l_buf || !l2h_buf || !h2l || !l2h) {
		fprintf(stderr, "[relay] alloc failed\n");
		return -1.0;
	}
	atomic_store(&h2l->head, 0);
	atomic_store(&h2l->tail, 0);
	atomic_store(&l2h->head, 0);
	atomic_store(&l2h->tail, 0);

	struct shmem_relay_attach_arg a = {
	    .sock_fd = (uint32_t)sock_fd,
	    .ring_cap = RING_CAP,
	    .h2l_buf = (uint64_t)(uintptr_t)h2l_buf,
	    .h2l_head = (uint64_t)(uintptr_t)&h2l->head,
	    .h2l_tail = (uint64_t)(uintptr_t)&h2l->tail,
	    .l2h_buf = (uint64_t)(uintptr_t)l2h_buf,
	    .l2h_head = (uint64_t)(uintptr_t)&l2h->head,
	    .l2h_tail = (uint64_t)(uintptr_t)&l2h->tail,
	};
	long ir = lkl_sys_ioctl(relay_fd, SHMEM_RELAY_ATTACH,
				(unsigned long)(uintptr_t)&a);
	if (ir < 0) {
		fprintf(stderr, "[relay] SHMEM_RELAY_ATTACH: %s\n",
			lkl_strerror((int)ir));
		lkl_sys_close(relay_fd);
		lkl_sys_close(sock_fd);
		return -1.0;
	}

	struct relay_ctx ctx = {
	    .l2h_buf = l2h_buf,
	    .l2h = l2h,
	    .cap = RING_CAP,
	    .target = target_bytes,
	    .relay_fd = relay_fd,
	    .done = 0,
	};
	pthread_t th;
	pthread_create(&th, NULL, relay_consumer, &ctx);
	pthread_join(th, NULL);

	/* Tear down: close relay first so kthread stops, then the socket
	 * (which the kthread already half-closed on its way out). */
	lkl_sys_close(relay_fd);
	lkl_sys_close(sock_fd);
	free(h2l_buf);
	free(l2h_buf);
	free(h2l);
	free(l2h);

	double mb = (double)ctx.total / (1024.0 * 1024.0);
	fprintf(stderr, "[relay] %.0f MB in %.3f s -> %.2f MB/s\n", mb,
		ctx.elapsed, mb / ctx.elapsed);
	return mb / ctx.elapsed;
}

struct stream_arg {
	uint16_t lkl_port;
	uint64_t target_bytes;
	double result_mbps;
};

static void* syscall_stream_thr(void* p)
{
	struct stream_arg* a = p;
	a->result_mbps = run_syscall_bench(a->lkl_port, a->target_bytes);
	return NULL;
}
static void* relay_stream_thr(void* p)
{
	struct stream_arg* a = p;
	a->result_mbps = run_relay_bench(a->lkl_port, a->target_bytes);
	return NULL;
}

static double run_parallel(const char* label, int n, uint64_t per_target,
			   void* (*fn)(void*))
{
	pthread_t* th = calloc(n, sizeof(*th));
	struct stream_arg* args = calloc(n, sizeof(*args));
	double t0 = now_sec();
	for (int i = 0; i < n; i++) {
		args[i].lkl_port = LKL_ZERO_PORT;
		args[i].target_bytes = per_target;
		pthread_create(&th[i], NULL, fn, &args[i]);
	}
	double sum = 0;
	for (int i = 0; i < n; i++) {
		pthread_join(th[i], NULL);
		if (args[i].result_mbps > 0)
			sum += args[i].result_mbps;
	}
	double t1 = now_sec();
	uint64_t total_mb = (per_target * n) >> 20;
	double agg = (double)total_mb / (t1 - t0);
	fprintf(
	    stderr,
	    "[bench] %s N=%d: sum-of-stream=%.2f MB/s  aggregate=%.2f MB/s\n",
	    label, n, sum, agg);
	free(th);
	free(args);
	return agg;
}

int main(int argc, char** argv)
{
	const char* mode = argc > 1 ? argv[1] : "both";
	uint64_t target_mb =
	    (argc > 2) ? (uint64_t)atoll(argv[2]) : DEFAULT_TARGET_MB;
	int n_streams = (argc > 3) ? atoi(argv[3]) : 1;
	uint64_t target_bytes = target_mb * 1024ULL * 1024ULL;

	signal(SIGPIPE, SIG_IGN);

	if (boot_lkl(LKL_ZERO_PORT) != 0) {
		fprintf(stderr, "LKL boot failed\n");
		return 1;
	}
	fprintf(
	    stderr,
	    "[bench] LKL up, zerobench writer on :%u, target=%llu MB x %d\n",
	    (unsigned)LKL_ZERO_PORT, (unsigned long long)target_mb, n_streams);

	double r_syscall = -1.0, r_relay = -1.0;
	if (!strcmp(mode, "syscall") || !strcmp(mode, "both")) {
		r_syscall =
		    (n_streams == 1)
			? run_syscall_bench(LKL_ZERO_PORT, target_bytes)
			: run_parallel("syscall", n_streams, target_bytes,
				       syscall_stream_thr);
	}
	if (!strcmp(mode, "relay") || !strcmp(mode, "both")) {
		r_relay = (n_streams == 1)
			      ? run_relay_bench(LKL_ZERO_PORT, target_bytes)
			      : run_parallel("relay", n_streams, target_bytes,
					     relay_stream_thr);
	}

	if (r_syscall > 0 && r_relay > 0) {
		fprintf(stderr,
			"[bench] summary: syscall=%.2f MB/s  relay=%.2f MB/s"
			"  (relay/syscall = %.3fx)\n",
			r_syscall, r_relay, r_relay / r_syscall);
	}

	lkl_sys_halt();
	lkl_cleanup();
	return 0;
}
