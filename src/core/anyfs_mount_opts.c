/*
 * anyfs_mount_opts.c — mount options anyfs adds per filesystem (internal)
 *
 * Pure string logic, kept apart from anyfs_mount.c so it builds and is
 * unit-tested without LKL.
 */
#include "anyfs_mount_opts.h"

#include <stdio.h>
#include <string.h>

/* Filesystems that take an errors= option. Only ext2/3/4 read their error
 * policy from the image (superblock s_errors and s_mount_opts, panic
 * included); ext4 applies the caller's mount options after both, so ours
 * win. For the others the option is extra caution. anyfs opens images users
 * did not make, so a read-only mount uses errors=continue (an error surfaces
 * as an error return, e.g. EUCLEAN/EBADMSG on ext4, and the kernel lives on)
 * and a read-write mount uses errors=remount-ro (stop writing to a
 * filesystem just found corrupt; this also clears ERRORS_PANIC).
 *
 * For NTFS PLUS on a read-write mount, remount-ro is also what turns on the
 * driver's own guards: a volume Windows hibernated, or one with a $MFTMirr
 * mismatch or LogFile/$Quota trouble, now mounts read-only instead of
 * read-write (oot-fs ntfsplus/super.c). The driver's own comment says it
 * must not write anything to a hibernated volume. */
static const char* const errors_opt_fs[] = {
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

int anyfs_mount_opts(const char* fstype, int rdonly, unsigned fat_cp, char* buf,
		     size_t cap)
{
	size_t len = 0;

	if (!buf || cap == 0)
		return -1;
	buf[0] = '\0';
	if (!fstype)
		return 0;

	if (rdonly) {
		/* Never replay a journal or log into a read-only image. */
		if (strcmp(fstype, "xfs") == 0 || strcmp(fstype, "btrfs") == 0) {
			if (append(buf, cap, &len, "norecovery"))
				goto overflow;
		} else if (strcmp(fstype, "ext4") == 0 ||
			   strcmp(fstype, "ext3") == 0) {
			if (append(buf, cap, &len, "noload"))
				goto overflow;
		}
	}
	/* The Linux UFS driver defaults to 44bsd UFS1, which matches nothing
	 * modern; FreeBSD, NetBSD and OpenBSD all ship UFS2. */
	if (strcmp(fstype, "ufs") == 0 &&
	    append(buf, cap, &len, "ufstype=ufs2"))
		goto overflow;
	/* FAT long names are UTF-16 on disk; without utf8 the kernel converts
	 * them with iocharset=iso8859-1 (CONFIG_FAT_DEFAULT_IOCHARSET), turning
	 * CJK into '?' and Latin-1 into bytes that are not UTF-8. utf8 rather
	 * than iocharset=utf8, which breaks case-insensitive lookup. Short (8.3)
	 * names are bytes in an OEM codepage: the caller picks it. */
	if (strcmp(fstype, "vfat") == 0 && append(buf, cap, &len, "utf8"))
		goto overflow;
	if (fat_cp &&
	    (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "msdos") == 0)) {
		char cp[24];

		snprintf(cp, sizeof(cp), "codepage=%u", fat_cp);
		if (append(buf, cap, &len, cp))
			goto overflow;
	}
	if (in_list(fstype, errors_opt_fs) &&
	    append(buf, cap, &len,
		   rdonly ? "errors=continue" : "errors=remount-ro"))
		goto overflow;
	return 0;

overflow:
	buf[0] = '\0';
	return -1;
}
