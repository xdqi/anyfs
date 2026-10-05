/*
 * macOS smoke test for the converted LKL kernel (liblkl-kernel.dylib) and the
 * Darwin host library (liblkl-host.a). Boots LKL, mounts an ext4 image
 * read-write, writes a file and reads it back, remounts the file system (which
 * drops its page cache) and reads the file again so the data really goes
 * through the block device, lists the directory, checks that a 100 ms sleep
 * inside the kernel takes about 100 ms, then unmounts and halts. Prints PASS
 * and exits 0 only if every step succeeds. A hang is cut off after 120 s.
 *
 * Usage: lkl-macos-smoke <ext4-image>   (the image is modified)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

#define TIMEOUT_S 120

static int nchecks;

#define OK(...)								\
	do {								\
		nchecks++;						\
		printf("ok   ");					\
		printf(__VA_ARGS__);					\
		putchar('\n');						\
	} while (0)

#define FAIL(...)							\
	do {								\
		fprintf(stderr, "FAIL: ");				\
		fprintf(stderr, __VA_ARGS__);				\
		fputc('\n', stderr);					\
		return 1;						\
	} while (0)

/* CHECK(label, cond, fmt, ...): prints "ok   label", or FAIL with fmt. */
#define CHECK(label, cond, ...)						\
	do {								\
		if (!(cond))						\
			FAIL(label ": " __VA_ARGS__);			\
		OK("%s", label);					\
	} while (0)

static void on_alarm(int sig)
{
	static const char m[] = "FAIL: timed out after 120 s (hang)\n";
	ssize_t r = write(2, m, sizeof(m) - 1);

	(void)sig;
	(void)r;
	_exit(1);
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

/* Reads path and compares it with msg. Returns 0 on success. */
static int read_check(const char *path, int flags, const char *what,
		      const char *msg, size_t len, int seek_first)
{
	char buf[64];
	long ret;
	int fd;

	fd = lkl_sys_open(path, flags, 0);
	if (fd < 0)
		FAIL("%s: open %s: %s", what, path, lkl_strerror(fd));
	if (seek_first) {
		long long off = lkl_sys_lseek(fd, 0, LKL_SEEK_SET);

		if (off != 0)
			FAIL("%s: lseek: %lld (%s)", what, off,
			     lkl_strerror((int)off));
	}
	memset(buf, 0, sizeof(buf));
	ret = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	if (ret != (long)len)
		FAIL("%s: read returned %ld (%s), expected %zu", what, ret,
		     ret < 0 ? lkl_strerror((int)ret) : "short read", len);
	if (memcmp(buf, msg, len) != 0)
		FAIL("%s: content mismatch, got '%s'", what, buf);
	ret = lkl_sys_close(fd);
	if (ret != 0)
		FAIL("%s: close: %ld (%s)", what, ret, lkl_strerror((int)ret));
	OK("%s: %zu bytes match", what, len);
	return 0;
}

static int run(const char *image)
{
	static const char msg[] = "hello from lkl on macos\n";
	const size_t len = sizeof(msg) - 1;
	struct __lkl__kernel_timespec nap = { 0, 100 * 1000 * 1000 };
	struct lkl_disk disk = { 0 };
	struct lkl_linux_dirent64 *de;
	struct timespec t0, t1;
	char mnt[64], path[96];
	struct lkl_dir *dir;
	int disk_id, fd, err = 0, found = 0;
	long ret, ms;

	CHECK("lkl_init", lkl_init(&lkl_host_ops) == 0, "failed");
	disk.fd = open(image, O_RDWR);
	if (disk.fd < 0)
		FAIL("open %s: %s", image, strerror(errno));
	OK("open %s", image);
	disk_id = lkl_disk_add(&disk);
	CHECK("lkl_disk_add", disk_id >= 0, "%s", lkl_strerror(disk_id));
	ret = lkl_start_kernel("mem=64M loglevel=8");
	CHECK("lkl_start_kernel", ret == 0, "%s", lkl_strerror(ret));

	ret = lkl_mount_dev(disk_id, 0, "ext4", 0, NULL, mnt, sizeof(mnt));
	CHECK("mount ext4 rw", ret == 0, "%s", lkl_strerror(ret));
	snprintf(path, sizeof(path), "%s/smoke.txt", mnt);

	fd = lkl_sys_open(path, LKL_O_CREAT | LKL_O_RDWR | LKL_O_TRUNC, 0644);
	CHECK("create smoke.txt", fd >= 0, "open %s: %s", path, lkl_strerror(fd));
	ret = lkl_sys_write(fd, msg, len);
	CHECK("write", ret == (long)len, "returned %ld (%s)", ret,
	      ret < 0 ? lkl_strerror((int)ret) : "short write");
	ret = lkl_sys_fsync(fd);
	CHECK("fsync", ret == 0, "%ld (%s)", ret, lkl_strerror((int)ret));
	{
		long long off = lkl_sys_lseek(fd, 0, LKL_SEEK_SET);

		CHECK("lseek", off == 0, "%lld (%s)", off,
		      lkl_strerror((int)off));
	}
	{
		char buf[64] = { 0 };

		ret = lkl_sys_read(fd, buf, sizeof(buf) - 1);
		CHECK("read back in place", ret == (long)len &&
		      memcmp(buf, msg, len) == 0,
		      "read %ld bytes: '%s'", ret, buf);
	}
	ret = lkl_sys_close(fd);
	CHECK("close", ret == 0, "%ld (%s)", ret, lkl_strerror((int)ret));

	/* Unmount + mount drops the page cache, so the next read hits the disk. */
	ret = lkl_umount_dev(disk_id, 0, 0, 1000);
	CHECK("umount (flush to disk)", ret == 0, "%s", lkl_strerror(ret));
	ret = lkl_mount_dev(disk_id, 0, "ext4", 0, NULL, mnt, sizeof(mnt));
	CHECK("remount ext4 rw", ret == 0, "%s", lkl_strerror(ret));
	snprintf(path, sizeof(path), "%s/smoke.txt", mnt);
	if (read_check(path, LKL_O_RDONLY, "read after remount", msg, len, 0))
		return 1;

	dir = lkl_opendir(mnt, &err);
	CHECK("opendir", dir != NULL, "%s: %s", mnt, lkl_strerror(err));
	while ((de = lkl_readdir(dir)))
		found |= strcmp(de->d_name, "smoke.txt") == 0;
	err = lkl_errdir(dir);
	lkl_closedir(dir);
	CHECK("readdir", err == 0, "%s: %s", mnt, lkl_strerror(err));
	CHECK("smoke.txt listed", found, "smoke.txt not found in %s", mnt);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	ret = lkl_sys_nanosleep(&nap, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK("nanosleep call", ret == 0, "%ld (%s)", ret,
	      lkl_strerror((int)ret));
	ms = elapsed_ms(&t0, &t1);
	if (ms < 95 || ms >= 1000)
		FAIL("nanosleep 100 ms took %ld ms (expected 95..999)", ms);
	OK("nanosleep 100 ms took %ld ms", ms);

	ret = lkl_umount_dev(disk_id, 0, 0, 1000);
	CHECK("umount", ret == 0, "%s", lkl_strerror(ret));
	ret = lkl_sys_halt();
	CHECK("halt", ret == 0, "returned %ld (%s)", ret,
	      lkl_strerror((int)ret));
	lkl_cleanup();
	close(disk.fd);
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;

	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);
	alarm(TIMEOUT_S);

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ext4-image>\n", argv[0]);
		return 2;
	}
	if (run(argv[1]))
		return 1;
	printf("PASS (%d checks)\n", nchecks);
	return 0;
}
