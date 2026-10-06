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
	char buf[64];
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
	expect("ext4", 0, "errors=continue");
	expect("ext3", 1, "noload,errors=continue");
	expect("ext2", 1, "errors=continue");
	expect("vfat", 1, "errors=continue");
	expect("msdos", 1, "errors=continue");
	expect("exfat", 1, "errors=continue");
	expect("f2fs", 1, "errors=continue");
	expect("ntfs", 1, "errors=continue");

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

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("mount_opts: all passed\n");
	return 0;
}
