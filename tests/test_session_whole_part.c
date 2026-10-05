/*
 * test_session_whole_part.c — switching between the whole disk and one of
 * its partitions on a disk that has both, like a hybrid ISO (an iso9660
 * whole disk with a FAT EFI partition in its MBR).
 *
 * The kernel cannot mount the whole disk and a partition of it at the same
 * time: both claim the block device exclusively, and the second mount fails
 * with EBUSY. Regressions covered:
 *   - EBUSY marked the partition FAILED for good, so it could never be
 *     entered again in that session;
 *   - whole-disk enter was not idempotent: entering it twice failed with
 *     EBUSY;
 *   - closing the session left a whole-disk mount behind;
 *   - ANYFS_MOUNT_REPLACE (the UI's one-mount-at-a-time mode) unmounts the
 *     conflicting mount instead of failing.
 *
 * The image is generated: sector 0 is a FAT12 filesystem spanning the
 * first 4 MiB that also carries an MBR, whose partition 1 holds a second
 * FAT12 filesystem at 4 MiB. Raw backend, read-only.
 */
#include "anyfs.h"

#include <lkl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTORS_PER_FS 8192u /* 4 MiB per filesystem */
#define PART_START SECTORS_PER_FS
#define IMG_SIZE (2u * SECTORS_PER_FS * 512u)

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			exit(1);                                               \
		}                                                              \
	} while (0)

static void put16(unsigned char* p, unsigned v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static void put32(unsigned char* p, unsigned v)
{
	put16(p, v & 0xffff);
	put16(p + 2, v >> 16);
}

/* Write an empty FAT12 filesystem of SECTORS_PER_FS sectors at `sector`:
 * 4-sector clusters (2048 of them), 2 FATs of 6 sectors, 16 root entries.
 * With `mbr`, the boot sector also carries a partition table whose entry 1
 * covers the second filesystem. */
static void write_fat12(int fd, unsigned sector, int mbr)
{
	unsigned char bs[512] = {0};
	bs[0] = 0xeb, bs[1] = 0x3c, bs[2] = 0x90;
	memcpy(bs + 3, "ANYFSTST", 8);
	put16(bs + 11, 512);	/* bytes per sector */
	bs[13] = 4;		/* sectors per cluster */
	put16(bs + 14, 1);	/* reserved sectors */
	bs[16] = 2;		/* FATs */
	put16(bs + 17, 16);	/* root entries */
	put16(bs + 19, SECTORS_PER_FS);
	bs[21] = 0xf8;		/* media */
	put16(bs + 22, 6);	/* sectors per FAT */
	put16(bs + 24, 32);	/* sectors per track */
	put16(bs + 26, 2);	/* heads */
	bs[38] = 0x29;		/* extended boot signature */
	put32(bs + 39, 0x1234abcd + sector);
	memcpy(bs + 43, "ANYFS TEST ", 11);
	memcpy(bs + 54, "FAT12   ", 8);
	if (mbr) {
		unsigned char* e = bs + 446; /* partition entry 1 */
		e[4] = 0x01;		     /* FAT12 */
		put32(e + 8, PART_START);
		put32(e + 12, SECTORS_PER_FS);
	}
	bs[510] = 0x55, bs[511] = 0xaa;
	CHECK(pwrite(fd, bs, sizeof(bs), (off_t)sector * 512) == sizeof(bs),
	      "write boot sector");

	unsigned char fat[3] = {0xf8, 0xff, 0xff};
	for (unsigned i = 0; i < 2; i++)
		CHECK(pwrite(fd, fat, sizeof(fat),
			     (off_t)(sector + 1 + 6 * i) * 512) == sizeof(fat),
		      "write FAT");
}

static int mounted(const char* what)
{
	char buf[8192];
	int fd = lkl_sys_open("/proc/mounts", LKL_O_RDONLY, 0);
	CHECK(fd >= 0, "open /proc/mounts");
	long n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	lkl_sys_close(fd);
	buf[n > 0 ? n : 0] = '\0';
	return strstr(buf, what) != NULL;
}

static int enter(AnyfsSession* s, unsigned part, uint32_t flags,
		 char out[ANYFS_LKL_PATH_MAX])
{
	int rc = anyfs_session_enter(s, part, ANYFS_MOUNT_RDONLY | flags, out);
	printf("enter %u%s -> %d %s\n", part,
	       flags & ANYFS_MOUNT_REPLACE ? " (replace)" : "", rc,
	       rc == 0 ? out : "");
	return rc;
}

int main(void)
{
	char img[64];
	snprintf(img, sizeof(img), "/tmp/anyfs-wholepart-XXXXXX");
	int fd = mkstemp(img);
	CHECK(fd >= 0 && ftruncate(fd, IMG_SIZE) == 0, "create image");
	write_fat12(fd, 0, 1);
	write_fat12(fd, PART_START, 0);
	close(fd);

	CHECK(anyfs_kernel_init(NULL) == 0, "kernel init");

	AnyfsSession* s = NULL;
	CHECK(anyfs_session_open(img, ANYFS_SESSION_READONLY | ANYFS_BACKEND_RAW,
				 &s) == 0,
	      "open: %s", anyfs_get_last_error() ? anyfs_get_last_error() : "?");
	CHECK(anyfs_session_count(s, -1) == 1, "expected 1 partition, got %zu",
	      anyfs_session_count(s, -1));

	char p1[ANYFS_LKL_PATH_MAX], whole[ANYFS_LKL_PATH_MAX];
	char again[ANYFS_LKL_PATH_MAX];

	/* Default (server) semantics: a conflict fails with EBUSY. */
	CHECK(enter(s, 1, 0, p1) == 0, "enter p1");
	CHECK(enter(s, 0, 0, whole) == -LKL_EBUSY,
	      "whole while p1 is mounted must fail with EBUSY");
	CHECK(anyfs_session_leave(s, 1) == 0, "leave p1");
	CHECK(enter(s, 0, 0, whole) == 0, "whole after leaving p1");
	CHECK(enter(s, 0, 0, again) == 0 && strcmp(whole, again) == 0,
	      "whole-disk enter must be idempotent");
	CHECK(enter(s, 1, 0, p1) == -LKL_EBUSY,
	      "p1 while the whole disk is mounted must fail with EBUSY");
	CHECK(anyfs_session_leave(s, 0) == 0, "leave whole");
	CHECK(enter(s, 1, 0, p1) == 0,
	      "p1 must be enterable again once the conflict is gone");

	/* ANYFS_MOUNT_REPLACE: the conflicting mount is unmounted instead. */
	CHECK(enter(s, 0, ANYFS_MOUNT_REPLACE, whole) == 0,
	      "whole with REPLACE while p1 is mounted");
	CHECK(!mounted(p1), "REPLACE must unmount p1");
	CHECK(enter(s, 1, ANYFS_MOUNT_REPLACE, p1) == 0,
	      "p1 with REPLACE while the whole disk is mounted");
	CHECK(!mounted(whole), "REPLACE must unmount the whole disk");
	CHECK(enter(s, 0, ANYFS_MOUNT_REPLACE, whole) == 0, "whole again");

	/* Close must not leave the whole-disk mount behind. */
	anyfs_session_close(s);
	CHECK(!mounted(whole), "close left %s mounted", whole);

	unlink(img);
	printf("ok\n");
	return 0;
}
