// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the per-filesystem mount options (src/core/anyfs_mount_opts.c). */
#include "anyfs_mount_opts.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

static void expect(const char *fstype, int rdonly, const char *want)
{
	char buf[ANYFS_MOUNT_OPTS_MAX];
	int rc = anyfs_mount_opts(fstype, rdonly, buf, sizeof(buf));

	if (rc != 0 || strcmp(buf, want) != 0) {
		fprintf(stderr, "FAIL %s rdonly=%d: rc=%d got \"%s\", want \"%s\"\n",
			fstype, rdonly, rc, buf, want);
		failures++;
	}
}

int main(void)
{
	char buf[8];

	/* A superblock must not be able to ask for a panic. */
	expect("ext4", 1, "noload,errors=continue");
	expect("ext3", 1, "noload,errors=continue");
	expect("ext2", 1, "errors=continue");
	expect("vfat", 1, "errors=continue");
	expect("msdos", 1, "errors=continue");
	expect("exfat", 1, "errors=continue");
	expect("f2fs", 1, "errors=continue");
	expect("ntfs", 1, "errors=continue");

	/* Read-write: stop writing to a filesystem found corrupt. */
	expect("ext2", 0, "errors=remount-ro");
	expect("ext3", 0, "errors=remount-ro");
	expect("ext4", 0, "errors=remount-ro");
	expect("vfat", 0, "errors=remount-ro");
	expect("msdos", 0, "errors=remount-ro");
	expect("exfat", 0, "errors=remount-ro");
	expect("f2fs", 0, "errors=remount-ro");
	expect("ntfs", 0, "errors=remount-ro");
	expect("xfs", 0, "");

	/* No errors= option: unchanged. */
	expect("xfs", 1, "norecovery");
	expect("btrfs", 1, "norecovery");
	expect("btrfs", 0, "");
	expect("iso9660", 1, "");
	expect("hfsplus", 1, "");
	expect("apfs", 1, "");
	expect("ufs", 1, "ufstype=ufs2");
	expect("ufs", 0, "ufstype=ufs2");

	CHECK(anyfs_mount_opts(NULL, 1, buf, sizeof(buf)) == 0 && buf[0] == '\0');
	/* Too small: fails and leaves an empty string. */
	CHECK(anyfs_mount_opts("ext4", 1, buf, sizeof(buf)) == -1 && buf[0] == '\0');
	CHECK(anyfs_mount_opts("ext4", 1, buf, 0) == -1);
	CHECK(anyfs_mount_opts("ext4", 1, NULL, 8) == -1);

	/* "noload,errors=continue" is 22 chars: exact fit needs cap 23. */
	{
		char b[ANYFS_MOUNT_OPTS_MAX];

		CHECK(strlen("noload,errors=continue") == 22);
		CHECK(anyfs_mount_opts("ext4", 1, b, 23) == 0 &&
		      strcmp(b, "noload,errors=continue") == 0);
		CHECK(anyfs_mount_opts("ext4", 1, b, 22) == -1 && b[0] == '\0');
	}

	/* Every fs in the matrix fits ANYFS_MOUNT_OPTS_MAX. */
	{
		static const char *const fs[] = {
			"ext2", "ext3", "ext4", "vfat", "msdos", "exfat", "f2fs",
			"ntfs", "xfs", "btrfs", "ufs", "iso9660", "apfs", NULL,
		};
		char b[ANYFS_MOUNT_OPTS_MAX];

		for (int i = 0; fs[i]; i++)
			for (int ro = 0; ro < 2; ro++)
				CHECK(anyfs_mount_opts(fs[i], ro, b, sizeof(b)) == 0);
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("mount_opts: all passed\n");
	return 0;
}
