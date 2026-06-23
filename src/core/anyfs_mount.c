/*
 * anyfs_mount.c — LKL filesystem mount / unmount logic (internal)
 */
#define _GNU_SOURCE
#include "anyfs_mount.h"
#include "anyfs.h"

#include <stdio.h>
#include <string.h>

/* Upper bound on the block filesystems pulled from /proc/filesystems for the
 * auto-probe mount loop. Must exceed the number of block (non-nodev) entries
 * the kernel registers, or fstypes past the cap are silently never tried — a
 * real bug: with the OOT drivers (ntfs/apfs/zfs) plus the full builtin set the
 * list is ~35 block entries, so a cap of 32 dropped ntfs/apfs/btrfs (they
 * appear last), making any such partition that blkid fails to hint unmountable.
 * 128 leaves comfortable headroom. The /proc/filesystems read buffer below is
 * sized to match. */
#define MAX_FSTYPES 128
#define FSTYPE_MAXLEN 32

/* Read /proc/filesystems from LKL and return block filesystem types */
static int get_block_fstypes(char fstypes[][FSTYPE_MAXLEN], int max)
{
	int fd = lkl_sys_open("/proc/filesystems", LKL_O_RDONLY, 0);
	if (fd < 0)
		return 0;

	char buf[8192];
	int n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	lkl_sys_close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';

	int count = 0;
	char* line = buf;
	while (line && *line && count < max) {
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		/* Lines starting with whitespace are block filesystems;
		 * lines starting with "nodev" are pseudo/network fs */
		if (line[0] == '\t' || line[0] == ' ') {
			char* name = line;
			while (*name == '\t' || *name == ' ')
				name++;
			if (*name && strlen(name) < FSTYPE_MAXLEN) {
				strncpy(fstypes[count], name,
					FSTYPE_MAXLEN - 1);
				fstypes[count][FSTYPE_MAXLEN - 1] = '\0';
				count++;
			}
		}

		line = nl ? nl + 1 : NULL;
	}
	return count;
}

/* Common path: dev_str already points to a /dev node (we may or may
 * not have mknod'd it). Returns 0 on success, negative on failure;
 * caller is responsible for any /dev/<encoded> node cleanup it owns. */
static int mount_via_devpath(const char* dev_str, const char* fstype,
			     const char* name, uint32_t flags, AnyfsMount* out)
{
	int auto_detect = (!fstype || strcmp(fstype, "auto") == 0);

	lkl_sys_mkdir("/lklmnt", 0755);
	char mnt[64];
	snprintf(mnt, sizeof(mnt), "/lklmnt/%s", name);
	lkl_sys_mkdir(mnt, 0755);

	int mount_flags = 0;
	if (flags & ANYFS_MOUNT_RDONLY)
		mount_flags |= LKL_MS_RDONLY;

	int ret;
	if (!auto_detect) {
		const char* opts = NULL;
		if (flags & ANYFS_MOUNT_RDONLY) {
			if (strcmp(fstype, "xfs") == 0 ||
			    strcmp(fstype, "btrfs") == 0)
				opts = "norecovery";
			else if (strcmp(fstype, "ext4") == 0 ||
				 strcmp(fstype, "ext3") == 0)
				opts = "noload";
		}
		// The Linux UFS driver needs ufstype= to pick the right
		// superblock layout. Default is 44bsd UFS1 (ufstype=old) which
		// fails on every modern image. FreeBSD/NetBSD/OpenBSD all ship
		// ufs2 today.
		if (strcmp(fstype, "ufs") == 0 && !opts)
			opts = "ufstype=ufs2";
		ret = lkl_sys_mount((char*)dev_str, mnt, (char*)fstype,
				    mount_flags, (char*)opts);
		if (ret < 0) {
			lkl_sys_rmdir(mnt);
			return ret;
		}
		strncpy(out->mount_point, mnt, sizeof(out->mount_point) - 1);
		out->mount_point[sizeof(out->mount_point) - 1] = '\0';
		strncpy(out->fstype, fstype, sizeof(out->fstype) - 1);
		out->fstype[sizeof(out->fstype) - 1] = '\0';
		return 0;
	}

	if (lkl_sys_access("/proc/filesystems", 0) < 0) {
		lkl_sys_mkdir("/proc", 0555);
		lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
	}

	char fstypes[MAX_FSTYPES][FSTYPE_MAXLEN];
	int nfs = get_block_fstypes(fstypes, MAX_FSTYPES);

	for (int i = 0; i < nfs; i++) {
		/* Skip apfs and btrfs in the BLIND probe loop. Both hold the
		 * block device across a *failed* mount (apfs's container open
		 * and btrfs's multi-device scan claim the device and don't
		 * release it on the failure path under LKL). Blind-probing them
		 * on a whole disk that has a partition table — e.g. when the
		 * user selects "whole disk #0" first — leaves the PARENT device
		 * (vda) held, and every subsequent partition mount (vda1, vda2,
		 * …) then fails with EBUSY. Both are reliably identified by
		 * blkid, so a real apfs/ btrfs volume always mounts via the
		 * explicit-fstype hint path above (not gated by this loop); the
		 * blind attempt is dead weight and only causes the device-busy
		 * regression. (ntfs is kept here: blkid often fails to type
		 * NTFS volumes, so the blind probe is the real path that mounts
		 * them, and NTFS PLUS does release the device on a failed
		 * probe.) */
		if (strcmp(fstypes[i], "apfs") == 0 ||
		    strcmp(fstypes[i], "btrfs") == 0)
			continue;
		const char* opts = NULL;
		if (flags & ANYFS_MOUNT_RDONLY) {
			if (strcmp(fstypes[i], "xfs") == 0 ||
			    strcmp(fstypes[i], "btrfs") == 0)
				opts = "norecovery";
			else if (strcmp(fstypes[i], "ext4") == 0 ||
				 strcmp(fstypes[i], "ext3") == 0)
				opts = "noload";
		}
		// ufstype=ufs2 covers modern FreeBSD/NetBSD/OpenBSD; the
		// default 44bsd layout doesn't match anything you'd actually
		// mount today.
		if (strcmp(fstypes[i], "ufs") == 0 && !opts)
			opts = "ufstype=ufs2";
		ret = lkl_sys_mount((char*)dev_str, mnt, fstypes[i],
				    mount_flags, (char*)opts);
		if (ret == 0) {
			strncpy(out->mount_point, mnt,
				sizeof(out->mount_point) - 1);
			out->mount_point[sizeof(out->mount_point) - 1] = '\0';
			strncpy(out->fstype, fstypes[i],
				sizeof(out->fstype) - 1);
			out->fstype[sizeof(out->fstype) - 1] = '\0';
			return 0;
		}
	}

	lkl_sys_rmdir(mnt);
	return -1;
}

int anyfs_mount(int disk_id, unsigned int part, const char* fstype,
		const char* name, uint32_t flags, AnyfsMount* out)
{
	if (!name || !out)
		return -1;

	uint32_t dev;
	int ret = lkl_get_virtio_blkdev(disk_id, part, &dev);
	if (ret < 0)
		return ret;

	char dev_str[32];
	snprintf(dev_str, sizeof(dev_str), "/dev/%08x", dev);
	lkl_sys_access("/dev", 0) < 0 && lkl_sys_mkdir("/dev", 0700);
	lkl_sys_mknod(dev_str, LKL_S_IFBLK | 0600, dev);

	ret = mount_via_devpath(dev_str, fstype, name, flags, out);
	if (ret < 0)
		lkl_sys_unlink(dev_str);
	return ret;
}

int anyfs_mount_blkdev(const char* dev_path, const char* fstype,
		       const char* name, uint32_t flags, AnyfsMount* out)
{
	if (!dev_path || !name || !out)
		return -1;
	/* The node is expected to already exist (caller mknod'd it). We
	 * do not unlink on failure — that's the session layer's job. */
	return mount_via_devpath(dev_path, fstype, name, flags, out);
}

int anyfs_umount(const char* name)
{
	if (!name)
		return -1;

	char mnt[64];
	snprintf(mnt, sizeof(mnt), "/lklmnt/%s", name);

	/* Flush all pending writes before unmount */
	lkl_sys_sync();

	int ret = lkl_sys_umount(mnt, 0);
	if (ret < 0)
		return ret;

	lkl_sys_rmdir(mnt);
	return 0;
}
