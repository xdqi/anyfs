/*
 * test_session_reopen.c — partitions must be listed correctly when disks
 * are closed and reopened within one kernel (the native/Electron model:
 * one process-global LKL kernel shared by every session).
 *
 * Regression: the session layer named the kernel block device
 * "vd" + ('a' + disk_id). LKL never reuses disk ids, while the kernel
 * reuses virtio-blk indexes once a removed disk is released — and names
 * the next disk "vdb" while it is not — so after any close the session
 * walked the wrong (or a missing) /sys/block entry and listed nothing.
 *
 * Images are generated: an 8 MiB disk with a one-partition MBR, and an
 * 8 MiB blank disk (no partition table). Raw backend; no filesystems
 * needed, since partition discovery only reads the table.
 */
#include "anyfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMG_SIZE (8u << 20)

static int make_image(char* path, size_t cap, int with_mbr)
{
	snprintf(path, cap, "/tmp/anyfs-reopen-XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, IMG_SIZE) != 0)
		return -1;
	if (with_mbr) {
		unsigned char mbr[512] = {0};
		unsigned char* e = mbr + 446; /* partition entry 1 */
		unsigned int start = 2048, count = IMG_SIZE / 512 - 2048;
		e[4] = 0x83; /* Linux */
		memcpy(e + 8, &start, 4);
		memcpy(e + 12, &count, 4);
		mbr[510] = 0x55;
		mbr[511] = 0xaa;
		if (pwrite(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr))
			return -1;
	}
	close(fd);
	return 0;
}

static int open_count_close(const char* path, const char* what)
{
	AnyfsSession* s = NULL;
	if (anyfs_session_open(path, ANYFS_SESSION_READONLY | ANYFS_BACKEND_RAW,
			       &s) != 0) {
		fprintf(stderr, "FAIL open %s: %s\n", what,
			anyfs_get_last_error() ? anyfs_get_last_error() : "?");
		exit(1);
	}
	int n = (int)anyfs_session_count(s, -1);
	anyfs_session_close(s);
	printf("%s: %d partition(s)\n", what, n);
	return n;
}

int main(void)
{
	char mbr[64], blank[64];
	if (make_image(mbr, sizeof(mbr), 1) ||
	    make_image(blank, sizeof(blank), 0)) {
		fprintf(stderr, "FAIL image setup\n");
		return 1;
	}
	if (anyfs_kernel_init(NULL) != 0) {
		fprintf(stderr, "FAIL kernel init\n");
		return 1;
	}

	int fails = 0;
	fails += open_count_close(blank, "blank #1") != 0;
	fails += open_count_close(mbr, "mbr #1 (after a close)") != 1;
	fails += open_count_close(mbr, "mbr #2 (after a close)") != 1;
	fails += open_count_close(blank, "blank #2") != 0;
	fails += open_count_close(mbr, "mbr #3 (after a close)") != 1;

	unlink(mbr);
	unlink(blank);
	if (fails) {
		fprintf(stderr,
			"FAIL %d reopen(s) listed the wrong partitions\n",
			fails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
