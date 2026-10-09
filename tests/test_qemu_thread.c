/*
 * test_qemu_thread.c — the dedicated QEMU thread embedding, tested below
 * LKL: the QEMU block backend is driven directly through qemu_blk_open /
 * qemu_blk_ops.request / qemu_blk_close.
 *
 * Usage: test_qemu_thread basic | watchdog
 *
 *   basic     concurrent reads from several host threads across two open
 *             disks, checked byte-for-byte against pread() of the same
 *             file; 100 open/close cycles; open errors reported on the
 *             calling thread; calls after shutdown fail fast.
 *   watchdog  an NBD server that accepts and never answers stalls
 *             blk_new_open on the QEMU thread; the watchdog must report it
 *             through the fatal hook (ANYFS_QEMU_TIMEOUT_MS=2000).
 *
 * The image is generated: an 8 MiB file of deterministic pseudo-random
 * bytes, opened as raw by QEMU's format probe.
 */
#include "anyfs.h"
#include "qemu_backend.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IMG_SIZE (8u << 20)
#define N_THREADS 8
#define N_READS 400
#define MAX_IOV 3

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			exit(1);                                               \
		}                                                              \
	} while (0)

static char g_img[64];
static int g_img_fd;

#ifndef __linux__
static void remove_image(void)
{
	unlink(g_img);
}
#endif

static void make_image(void)
{
	snprintf(g_img, sizeof(g_img), "/tmp/anyfs-qt-XXXXXX");
	g_img_fd = mkstemp(g_img);
	CHECK(g_img_fd >= 0, "mkstemp");
#ifdef __linux__
	unlink(g_img); /* fd keeps it alive; QEMU opens /proc/self/fd/N */
	snprintf(g_img, sizeof(g_img), "/proc/self/fd/%d", g_img_fd);
#else
	atexit(remove_image); /* no /proc elsewhere: QEMU opens the name */
#endif

	uint32_t x = 0x12345678u;
	static uint8_t chunk[1 << 16];
	for (uint32_t off = 0; off < IMG_SIZE; off += sizeof(chunk)) {
		for (size_t i = 0; i < sizeof(chunk); i++) {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			chunk[i] = (uint8_t)x;
		}
		CHECK(pwrite(g_img_fd, chunk, sizeof(chunk), off) ==
			  (ssize_t)sizeof(chunk),
		      "pwrite image");
	}
}

static void open_disk(struct lkl_disk* disk)
{
	CHECK(qemu_blk_open(g_img, ANYFS_SESSION_READONLY, disk) == 0,
	      "qemu_blk_open(%s): %s", g_img,
	      anyfs_get_last_error() ? anyfs_get_last_error() : "?");
	unsigned long long cap = 0;
	disk->ops->get_capacity(*disk, &cap);
	CHECK(cap == IMG_SIZE, "capacity %llu != %u", cap, IMG_SIZE);
}

/* Read [sector, sector + total) as up to MAX_IOV buffers via the backend
 * and compare with pread(). */
static void read_and_compare(struct lkl_disk disk, unsigned long long sector,
			     size_t* lens, int count)
{
	uint8_t got[MAX_IOV][16384], want[16384];
	struct iovec iov[MAX_IOV];
	for (int i = 0; i < count; i++) {
		iov[i].iov_base = got[i];
		iov[i].iov_len = lens[i];
	}
	struct lkl_blk_req req = {
	    .type = LKL_DEV_BLK_TYPE_READ,
	    .sector = sector,
	    .buf = iov,
	    .count = count,
	};
	CHECK(disk.ops->request(disk, &req) == LKL_DEV_BLK_STATUS_OK,
	      "request sector=%llu", sector);

	off_t off = (off_t)sector * 512;
	for (int i = 0; i < count; i++) {
		CHECK(pread(g_img_fd, want, lens[i], off) == (ssize_t)lens[i],
		      "pread");
		CHECK(memcmp(got[i], want, lens[i]) == 0,
		      "data mismatch at sector %llu buffer %d", sector, i);
		off += lens[i];
	}
}

struct reader_arg {
	struct lkl_disk disk;
	unsigned int seed;
};

static void* reader(void* p)
{
	struct reader_arg* a = p;
	for (int n = 0; n < N_READS; n++) {
		size_t lens[MAX_IOV], total = 0;
		int count = 1 + (int)(rand_r(&a->seed) % MAX_IOV);
		for (int i = 0; i < count; i++) {
			lens[i] = 512u * (1 + rand_r(&a->seed) % 32);
			total += lens[i];
		}
		unsigned long long max_sector = (IMG_SIZE - total) / 512;
		unsigned long long sector = rand_r(&a->seed) % (max_sector + 1);
		read_and_compare(a->disk, sector, lens, count);
	}
	return NULL;
}

static void test_concurrent_reads(void)
{
	struct lkl_disk disks[2];
	pthread_t th[N_THREADS];
	struct reader_arg args[N_THREADS];

	open_disk(&disks[0]);
	open_disk(&disks[1]);
	for (int i = 0; i < N_THREADS; i++) {
		args[i].disk = disks[i % 2];
		args[i].seed = 1000u + (unsigned)i;
		CHECK(pthread_create(&th[i], NULL, reader, &args[i]) == 0,
		      "pthread_create");
	}
	for (int i = 0; i < N_THREADS; i++)
		pthread_join(th[i], NULL);
	qemu_blk_close(&disks[0]);
	qemu_blk_close(&disks[1]);
	printf("ok concurrent reads (%d threads x %d)\n", N_THREADS, N_READS);
}

static void test_open_close_cycles(void)
{
	for (int i = 0; i < 100; i++) {
		struct lkl_disk disk;
		size_t len = 4096;
		open_disk(&disk);
		read_and_compare(disk, (unsigned long long)i * 8, &len, 1);
		qemu_blk_close(&disk);
		CHECK(disk.handle == NULL, "close left a handle");
	}
	printf("ok 100 open/close cycles\n");
}

static void test_open_error(void)
{
	struct lkl_disk disk;
	anyfs_set_last_error("%s", "");
	CHECK(qemu_blk_open("/nonexistent/anyfs-qt.qcow2",
			    ANYFS_SESSION_READONLY, &disk) < 0,
	      "open of a missing file succeeded");
	const char* err = anyfs_get_last_error();
	CHECK(err && strstr(err, "nonexistent"),
	      "error not reported on the calling thread: %s",
	      err ? err : "(null)");
	printf("ok open error reported: %s\n", err);
}

static void test_after_shutdown(void)
{
	struct lkl_disk disk;
	struct timespec t0, t1;

	qemu_backend_shutdown();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(qemu_blk_open(g_img, ANYFS_SESSION_READONLY, &disk) < 0,
	      "open after shutdown succeeded");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK(t1.tv_sec - t0.tv_sec < 2, "open after shutdown was slow");
	printf("ok open after shutdown fails fast: %s\n",
	       anyfs_get_last_error());
}

/* ── watchdog ─────────────────────────────────────────────────────── */

static pthread_mutex_t g_fatal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fatal_cond = PTHREAD_COND_INITIALIZER;
static char g_fatal_reason[512];

static void fatal_hook(const char* reason)
{
	pthread_mutex_lock(&g_fatal_lock);
	snprintf(g_fatal_reason, sizeof(g_fatal_reason), "%s", reason);
	pthread_cond_signal(&g_fatal_cond);
	pthread_mutex_unlock(&g_fatal_lock);
}

static void* silent_server(void* p)
{
	int lfd = *(int*)p;
	for (;;) {
		int c = accept(lfd, NULL, NULL);
		(void)c; /* hold the connection open, never send a greeting */
	}
	return NULL;
}

static void* stalled_open(void* p)
{
	struct lkl_disk disk;
	qemu_blk_open(p, ANYFS_SESSION_READONLY, &disk);
	return NULL;
}

static void test_watchdog(void)
{
	static char path[32];
	struct sockaddr_in sa = {.sin_family = AF_INET};
	socklen_t sl = sizeof(sa);
	pthread_t srv, opener;
	static int lfd;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(lfd >= 0 && bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) == 0 &&
		  listen(lfd, 4) == 0 &&
		  getsockname(lfd, (struct sockaddr*)&sa, &sl) == 0,
	      "listen");
	snprintf(path, sizeof(path), "nbd-port:%u", ntohs(sa.sin_port));
	pthread_create(&srv, NULL, silent_server, &lfd);

	anyfs_set_fatal_hook(fatal_hook);
	pthread_create(&opener, NULL, stalled_open, path);

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 20;
	pthread_mutex_lock(&g_fatal_lock);
	while (!g_fatal_reason[0])
		if (pthread_cond_timedwait(&g_fatal_cond, &g_fatal_lock,
					   &deadline) != 0)
			break;
	pthread_mutex_unlock(&g_fatal_lock);
	CHECK(g_fatal_reason[0], "fatal hook not called within 20 s");
	printf("ok watchdog: %s\n", g_fatal_reason);
	/* The opener and the QEMU thread stay blocked by design; leave
	 * without running atexit handlers. */
	fflush(stdout);
	_exit(0);
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s basic|watchdog\n", argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "watchdog") == 0) {
		setenv("ANYFS_QEMU_TIMEOUT_MS", "2000", 1);
		test_watchdog();
	}
	CHECK(strcmp(argv[1], "basic") == 0, "unknown mode %s", argv[1]);
	make_image();
	test_concurrent_reads();
	test_open_close_cycles();
	test_open_error();
	test_after_shutdown();
	return 0;
}
