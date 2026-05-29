/*
 * bench_backends.c — Benchmark different anyfs backends (raw, qemu, gio)
 *
 * Reads every block of a mounted filesystem image and reports throughput.
 * Usage: bench_backends <image> <fstype> [part]
 *
 * Automatically tests all compiled-in backends.
 */
#include "anyfs.h"
#include "anyfs_backend.h"
#include <lkl/asm-generic/fcntl.h>
#include <lkl/linux/mount.h>
#include <lkl/linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct bench_result {
	const char* backend;
	double elapsed_ms;
	uint64_t bytes_read;
};

static double time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Read all files recursively under mount_point, return total bytes */
static uint64_t read_all_files(const char* dirpath, char* buf, size_t bufsz)
{
	uint64_t total = 0;
	int err;
	struct lkl_dir* dir = lkl_opendir(dirpath, &err);
	if (!dir)
		return 0;

	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;

		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);

		if (de->d_type == 8) { /* regular file */
			long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
			if (fd >= 0) {
				long n;
				while ((n = lkl_sys_read(fd, buf, bufsz)) > 0)
					total += (uint64_t)n;
				lkl_sys_close(fd);
			}
		} else if (de->d_type == 4) { /* directory */
			total += read_all_files(path, buf, bufsz);
		}
	}
	lkl_closedir(dir);
	return total;
}

static int bench_one(const char* image, const char* fstype, uint32_t part,
		     uint32_t backend_flag, const char* backend_name,
		     struct bench_result* result)
{
	uint32_t flags = ANYFS_DISK_READONLY | backend_flag;
	int disk_id = anyfs_disk_add(image, flags);
	if (disk_id < 0) {
		fprintf(stderr, "  [%s] disk_add failed\n", backend_name);
		return -1;
	}

	char mount_point[32];
	const char* opts = (strcmp(fstype, "xfs") == 0) ? "norecovery" : NULL;
	long ret = lkl_mount_dev(disk_id, part, fstype, LKL_MS_RDONLY, opts,
				 mount_point, sizeof(mount_point));
	if (ret) {
		fprintf(stderr, "  [%s] mount failed: %ld\n", backend_name,
			ret);
		anyfs_disk_remove(disk_id);
		return -1;
	}

	char* buf = malloc(65536);
	if (!buf) {
		ret = -1;
		goto umount;
	}

	double t0 = time_ms();
	uint64_t bytes = read_all_files(mount_point, buf, 65536);
	double t1 = time_ms();

	free(buf);

	result->backend = backend_name;
	result->elapsed_ms = t1 - t0;
	result->bytes_read = bytes;

umount:
	lkl_umount_dev(disk_id, part, 0, 1000);
	anyfs_disk_remove(disk_id);
	return (ret == 0) ? 0 : -1;
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <image> <fstype> [part]\n", argv[0]);
		return 1;
	}

	const char* image = argv[1];
	const char* fstype = argv[2];
	uint32_t part = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;

	if (anyfs_kernel_init(NULL) < 0) {
		fprintf(stderr, "kernel init failed\n");
		return 1;
	}

	struct {
		const char* name;
		uint32_t flag;
		int available;
	} backends[] = {
	    {"raw", ANYFS_BACKEND_RAW, 1},
#ifdef ANYFS_HAS_QEMU
	    {"qemu", ANYFS_BACKEND_QEMU, 1},
#endif
#ifdef ANYFS_HAS_GIO
	    {"gio", ANYFS_BACKEND_GIO, 1},
#endif
	};
	int n_backends = sizeof(backends) / sizeof(backends[0]);

	printf("Benchmarking: %s (fs=%s, part=%u)\n", image, fstype, part);
	printf("%-8s %12s %12s %12s\n", "Backend", "Bytes", "Time(ms)", "MB/s");
	printf("-------- ------------ ------------ ------------\n");

	for (int i = 0; i < n_backends; i++) {
		if (!backends[i].available)
			continue;

		struct bench_result r = {0};
		if (bench_one(image, fstype, part, backends[i].flag,
			      backends[i].name, &r) == 0) {
			double mbps = (r.elapsed_ms > 0)
					  ? (r.bytes_read / (1024.0 * 1024.0)) /
						(r.elapsed_ms / 1000.0)
					  : 0;
			printf("%-8s %12llu %12.1f %12.1f\n", r.backend,
			       (unsigned long long)r.bytes_read, r.elapsed_ms,
			       mbps);
		}
	}

	anyfs_kernel_halt();
	return 0;
}
