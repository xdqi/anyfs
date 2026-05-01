/*
 * bench_raw_io.c - CrystalDiskMark-style raw I/O benchmark for LKL backends.
 *
 * Reads directly via LKL's /dev/vda (no filesystem mount).
 * Tests: sequential read, random read (4K blocks).
 * Compares: raw (pread), aio (threadless async).
 *
 * Usage: sudo ./bench_raw_io <block_device_or_image> [block_size_kb] [total_mb]
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "src/core/aio_blk_backend.h"
#include <lkl.h>
#include <lkl_host.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Simple xorshift64 PRNG for random offsets */
static uint64_t rng_state = 0x123456789ABCDEF0ULL;
static uint64_t xorshift64(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

static int64_t get_device_size_bytes(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	int64_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0) {
		/* block device: try ioctl */
		uint64_t sz;
		if (ioctl(fd, 0x80081272 /* BLKGETSIZE64 */, &sz) == 0)
			size = (int64_t)sz;
	}
	close(fd);
	return size;
}

struct bench_result {
	double elapsed_ms;
	int64_t bytes_read;
	int ops;
};

static struct bench_result do_bench(int lkl_fd, int64_t dev_size,
				    int block_size, int total_ops,
				    int sequential)
{
	struct bench_result res = {0};
	char* buf = aligned_alloc(4096, block_size);
	if (!buf) {
		fprintf(stderr, "OOM\n");
		exit(1);
	}

	int64_t max_offset = dev_size - block_size;
	if (max_offset < 0)
		max_offset = 0;
	int64_t blocks = max_offset / block_size;

	double start = now_ms();
	for (int i = 0; i < total_ops; i++) {
		int64_t offset;
		if (sequential) {
			offset = (int64_t)i * block_size;
			if (offset > max_offset)
				offset = offset % (max_offset + 1);
		} else {
			offset =
			    (int64_t)(xorshift64() % (uint64_t)(blocks + 1)) *
			    block_size;
		}
		long r = lkl_sys_pread64(lkl_fd, buf, block_size, offset);
		if (r < 0) {
			fprintf(stderr, "pread64 at offset %ld failed: %ld\n",
				offset, r);
			break;
		}
		res.bytes_read += r;
		res.ops++;
	}
	res.elapsed_ms = now_ms() - start;

	free(buf);
	return res;
}

static void print_result(const char* label, struct bench_result* r)
{
	double secs = r->elapsed_ms / 1000.0;
	double mb = r->bytes_read / (1024.0 * 1024.0);
	double mbps = secs > 0 ? mb / secs : 0;
	double iops = secs > 0 ? r->ops / secs : 0;
	printf("  %-20s | %6d ops | %8.2f ms | %8.2f MB/s | %8.0f IOPS\n",
	       label, r->ops, r->elapsed_ms, mbps, iops);
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <raw|aio> <device_or_image> [block_kb=4] "
			"[total_mb=16]\n",
			argv[0]);
		return 1;
	}

	const char* backend = argv[1];
	const char* path = argv[2];
	int block_kb = argc > 3 ? atoi(argv[3]) : 4;
	int total_mb = argc > 4 ? atoi(argv[4]) : 16;
	int block_size = block_kb * 1024;
	int total_ops = (total_mb * 1024 * 1024) / block_size;
	int use_aio = (strcmp(backend, "aio") == 0);

	int64_t dev_size = get_device_size_bytes(path);
	if (dev_size <= 0) {
		fprintf(stderr, "Cannot determine size of %s\n", path);
		return 1;
	}

	printf("Raw I/O Benchmark: %s (backend=%s)\n", path, backend);
	printf("  Device size: %.1f MB, Block: %d KB, Total: %d MB (%d ops)\n",
	       dev_size / (1024.0 * 1024.0), block_kb, total_mb, total_ops);
	printf("  %-20s | %6s     | %8s    | %8s      | %8s\n", "Test", "Ops",
	       "Time", "Throughput", "IOPS");
	printf("  "
	       "---------------------------------------------------------------"
	       "--------\n");

	struct lkl_disk disk = {0};
	if (use_aio) {
		disk.fd = open(path, O_RDONLY | O_DIRECT);
		if (disk.fd < 0)
			disk.fd = open(path, O_RDONLY);
		disk.ops = &aio_blk_ops;
	} else {
		disk.fd = open(path, O_RDONLY);
	}
	if (disk.fd < 0) {
		perror("open");
		return 1;
	}

	lkl_init(&lkl_host_ops);

	int disk_id = lkl_disk_add(&disk);
	if (disk_id < 0) {
		fprintf(stderr, "lkl_disk_add failed\n");
		return 1;
	}

	lkl_start_kernel("mem=64M");

	/* Mount proc to discover virtio-blk major number */
	lkl_mount_fs("proc");
	int proc_fd = lkl_sys_open("/proc/devices", LKL_O_RDONLY, 0);
	char procbuf[4096] = {0};
	if (proc_fd >= 0) {
		lkl_sys_read(proc_fd, procbuf, sizeof(procbuf) - 1);
		lkl_sys_close(proc_fd);
	}
	int vblk_major = 0;
	char* p = strstr(procbuf, "virtblk");
	if (p) {
		while (p > procbuf && p[-1] != '\n')
			p--;
		vblk_major = atoi(p);
	}
	if (vblk_major <= 0) {
		fprintf(stderr, "Cannot find virtblk major in /proc/devices\n");
		lkl_sys_halt();
		return 1;
	}

	lkl_sys_mkdir("/dev", 0755);
	char dev_path[64];
	snprintf(dev_path, sizeof(dev_path), "/dev/vd%c", 'a' + disk_id);
	unsigned int devnum =
	    (0 & 0xff) | (vblk_major << 8) | ((0 & ~0xff) << 12);
	lkl_sys_mknod(dev_path, LKL_S_IFBLK | 0600, devnum);

	int lkl_fd = lkl_sys_open(dev_path, LKL_O_RDONLY, 0);
	if (lkl_fd < 0) {
		fprintf(stderr, "lkl_sys_open(%s) failed: %d (major=%d)\n",
			dev_path, lkl_fd, vblk_major);
		lkl_sys_halt();
		return 1;
	}

	rng_state = 0x123456789ABCDEF0ULL;
	struct bench_result seq =
	    do_bench(lkl_fd, dev_size, block_size, total_ops, 1);
	print_result(use_aio ? "[aio] seq read" : "[raw] seq read", &seq);

	rng_state = 0x123456789ABCDEF0ULL;
	struct bench_result rnd =
	    do_bench(lkl_fd, dev_size, block_size, total_ops, 0);
	print_result(use_aio ? "[aio] rnd read" : "[raw] rnd read", &rnd);

	lkl_sys_close(lkl_fd);
	lkl_sys_halt();
	close(disk.fd);
	return 0;
}
