/*
 * anyfs_mount_opts.c — mount options anyfs adds per filesystem (internal)
 *
 * Pure string logic, kept apart from anyfs_mount.c so it builds and is
 * unit-tested without LKL.
 */
#include "anyfs_mount_opts.h"

#include <stdio.h>
#include <string.h>

/* Filesystems whose on-disk state can choose the kernel's reaction to an
 * error, panic included (ext*'s superblock s_errors; the others take the
 * option too). anyfs opens images users did not make, so it always mounts
 * these with errors=continue: the error surfaces as EIO and the kernel
 * lives on. */
static const char* const errors_continue_fs[] = {
	"ext2", "ext3", "ext4", "vfat", "msdos", "exfat", "f2fs", "ntfs", NULL,
};

static int in_list(const char* fstype, const char* const* list)
{
	for (; *list; list++)
		if (strcmp(fstype, *list) == 0)
			return 1;
	return 0;
}

static int append(char* buf, size_t cap, size_t* len, const char* opt)
{
	int n = snprintf(buf + *len, cap - *len, "%s%s", *len ? "," : "", opt);

	if (n < 0 || (size_t)n >= cap - *len)
		return -1;
	*len += (size_t)n;
	return 0;
}

int anyfs_mount_opts(const char* fstype, int rdonly, char* buf, size_t cap)
{
	size_t len = 0;
	int rc = 0;

	if (!buf || cap == 0)
		return -1;
	buf[0] = '\0';
	if (!fstype)
		return 0;

	if (rdonly) {
		/* Never replay a journal or log into a read-only image. */
		if (strcmp(fstype, "xfs") == 0 || strcmp(fstype, "btrfs") == 0)
			rc |= append(buf, cap, &len, "norecovery");
		else if (strcmp(fstype, "ext4") == 0 ||
			 strcmp(fstype, "ext3") == 0)
			rc |= append(buf, cap, &len, "noload");
	}
	/* The Linux UFS driver defaults to 44bsd UFS1, which matches nothing
	 * modern; FreeBSD, NetBSD and OpenBSD all ship UFS2. */
	if (strcmp(fstype, "ufs") == 0)
		rc |= append(buf, cap, &len, "ufstype=ufs2");
	if (in_list(fstype, errors_continue_fs))
		rc |= append(buf, cap, &len, "errors=continue");

	if (rc) {
		buf[0] = '\0';
		return -1;
	}
	return 0;
}
