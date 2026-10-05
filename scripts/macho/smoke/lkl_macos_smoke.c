/*
 * macOS smoke test for the converted LKL kernel (liblkl-kernel.dylib) and the
 * Darwin host library (liblkl-host.a). Boots LKL, mounts an ext4 image
 * read-write, writes a file and reads it back, lists the directory, checks
 * that a 100 ms sleep inside the kernel takes about 100 ms, then unmounts and
 * halts. Prints PASS and exits 0 only if every step succeeds.
 *
 * Usage: lkl-macos-smoke <ext4-image>   (the image is modified)
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL: " __VA_ARGS__);		\
			fputc('\n', stderr);				\
			return 1;					\
		}							\
		printf("ok   %s\n", #cond);				\
	} while (0)

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

static int run(const char *image)
{
	static const char msg[] = "hello from lkl on macos\n";
	struct __lkl__kernel_timespec nap = { 0, 100 * 1000 * 1000 };
	struct lkl_disk disk = { 0 };
	struct lkl_linux_dirent64 *de;
	struct timespec t0, t1;
	char mnt[64], path[96], buf[64];
	struct lkl_dir *dir;
	int disk_id, fd, err = 0, found = 0;
	long ret, ms;

	CHECK(lkl_init(&lkl_host_ops) == 0, "lkl_init");
	disk.fd = open(image, O_RDWR);
	CHECK(disk.fd >= 0, "open %s", image);
	disk_id = lkl_disk_add(&disk);
	CHECK(disk_id >= 0, "lkl_disk_add: %s", lkl_strerror(disk_id));
	ret = lkl_start_kernel("mem=64M loglevel=4");
	CHECK(ret == 0, "lkl_start_kernel: %s", lkl_strerror(ret));

	ret = lkl_mount_dev(disk_id, 0, "ext4", 0, NULL, mnt, sizeof(mnt));
	CHECK(ret == 0, "lkl_mount_dev: %s", lkl_strerror(ret));
	snprintf(path, sizeof(path), "%s/smoke.txt", mnt);

	fd = lkl_sys_open(path, LKL_O_CREAT | LKL_O_RDWR | LKL_O_TRUNC, 0644);
	CHECK(fd >= 0, "open %s: %s", path, lkl_strerror(fd));
	CHECK(lkl_sys_write(fd, msg, sizeof(msg) - 1) == sizeof(msg) - 1, "write");
	CHECK(lkl_sys_fsync(fd) == 0, "fsync");
	CHECK(lkl_sys_lseek(fd, 0, LKL_SEEK_SET) == 0, "lseek");
	memset(buf, 0, sizeof(buf));
	ret = lkl_sys_read(fd, buf, sizeof(buf));
	CHECK(ret == sizeof(msg) - 1 && memcmp(buf, msg, sizeof(msg) - 1) == 0,
	      "read back %ld bytes: '%s'", ret, buf);
	CHECK(lkl_sys_close(fd) == 0, "close");

	dir = lkl_opendir(mnt, &err);
	CHECK(dir != NULL, "opendir %s: %s", mnt, lkl_strerror(err));
	while ((de = lkl_readdir(dir)))
		found |= strcmp(de->d_name, "smoke.txt") == 0;
	lkl_closedir(dir);
	CHECK(found, "smoke.txt not listed in %s", mnt);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(lkl_sys_nanosleep(&nap, NULL) == 0, "nanosleep");
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = elapsed_ms(&t0, &t1);
	CHECK(ms >= 95 && ms < 1000, "a 100 ms sleep took %ld ms", ms);

	ret = lkl_umount_dev(disk_id, 0, 0, 1000);
	CHECK(ret == 0, "lkl_umount_dev: %s", lkl_strerror(ret));
	lkl_sys_halt();
	lkl_cleanup();
	close(disk.fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <ext4-image>\n", argv[0]);
		return 2;
	}
	if (run(argv[1]))
		return 1;
	printf("PASS\n");
	return 0;
}
