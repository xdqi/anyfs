/*
 * anyfs.c — Minimal LKL filesystem access: kernel init + disk management
 */
#define _GNU_SOURCE
#include "anyfs.h"
#include "anyfs_backend.h"
#include "raw_blk_backend.h"
#ifdef ANYFS_HAS_GIO
#include "gio_blk_backend.h"
#endif
#ifdef ANYFS_HAS_QEMU
#include "qemu_blk_backend.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal state ────────────────────────────────────────────── */

/* Backend registry */
const struct anyfs_backend_ops* anyfs_backends[ANYFS_MAX_BACKENDS];
int anyfs_backend_count;

void anyfs_register_backend(const struct anyfs_backend_ops* ops)
{
	if (anyfs_backend_count < ANYFS_MAX_BACKENDS)
		anyfs_backends[anyfs_backend_count++] = ops;
}

const struct anyfs_backend_ops* anyfs_find_backend(const char* name)
{
	for (int i = 0; i < anyfs_backend_count; i++)
		if (strcmp(anyfs_backends[i]->name, name) == 0)
			return anyfs_backends[i];
	return NULL;
}

#define MAX_DISKS 16

struct disk_slot {
	int in_use;
	int disk_id;
	struct lkl_disk disk;
	const struct anyfs_backend_ops* backend;
};

static struct disk_slot g_disks[MAX_DISKS];
static int g_kernel_started;

/* ── Kernel lifecycle ─────────────────────────────────────────── */

int anyfs_kernel_init(const AnyfsKernelOpts* opts)
{
	if (g_kernel_started)
		return 0;

	uint32_t mem_mb = 64;
	uint32_t loglevel = 0;
	if (opts) {
		if (opts->mem_mb)
			mem_mb = opts->mem_mb;
		loglevel = opts->loglevel;
	}

	/* Register backends */
	anyfs_register_backend(&raw_backend_ops);
#ifdef ANYFS_HAS_GIO
	anyfs_register_backend(&gio_backend_ops);
#endif
#ifdef ANYFS_HAS_QEMU
	anyfs_register_backend(&qemu_backend_ops);
#endif

	int ret = lkl_init(&lkl_host_ops);
	if (ret)
		return -1;

	char boot_args[128];
	snprintf(boot_args, sizeof(boot_args), "mem=%uM loglevel=%u", mem_mb,
		 loglevel);
	ret = lkl_start_kernel(boot_args);
	if (ret)
		return -1;

	g_kernel_started = 1;
	return 0;
}

void anyfs_kernel_halt(void)
{
	if (!g_kernel_started)
		return;
	lkl_sys_halt();
	lkl_cleanup();
	g_kernel_started = 0;
}

/* ── Disk management ──────────────────────────────────────────── */

int anyfs_disk_add(const char* image_path, uint32_t flags)
{
	if (!image_path || !g_kernel_started)
		return -1;

	/* Find free slot */
	int slot = -1;
	for (int i = 0; i < MAX_DISKS; i++) {
		if (!g_disks[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -1;

	int readonly = (flags & ANYFS_DISK_READONLY) ? 1 : 0;

	/* Select backend */
	const struct anyfs_backend_ops* ops = NULL;
#ifdef ANYFS_HAS_QEMU
	if (flags & ANYFS_BACKEND_QEMU)
		ops = &qemu_backend_ops;
#endif
#ifdef ANYFS_HAS_GIO
	if (!ops && (flags & ANYFS_BACKEND_GIO))
		ops = &gio_backend_ops;
#endif
	if (!ops && (flags & ANYFS_BACKEND_RAW))
		ops = &raw_backend_ops;

	/* Auto-detect: prefer QEMU if available, else raw */
	if (!ops) {
#ifdef ANYFS_HAS_QEMU
		ops = &qemu_backend_ops;
#else
		ops = &raw_backend_ops;
#endif
	}

	struct lkl_disk disk;
	int ret = ops->open(image_path, readonly, &disk);
	if (ret < 0)
		return -1;

	int disk_id = lkl_disk_add(&disk);
	if (disk_id < 0) {
		ops->close(&disk);
		return -1;
	}

	g_disks[slot].in_use = 1;
	g_disks[slot].disk_id = disk_id;
	g_disks[slot].disk = disk;
	g_disks[slot].backend = ops;
	return disk_id;
}

int anyfs_disk_remove(int disk_id)
{
	for (int i = 0; i < MAX_DISKS; i++) {
		if (g_disks[i].in_use && g_disks[i].disk_id == disk_id) {
			lkl_disk_remove(g_disks[i].disk);
			g_disks[i].backend->close(&g_disks[i].disk);
			g_disks[i].in_use = 0;
			return 0;
		}
	}
	return -1;
}

/* ── Mount management ─────────────────────────────────────────── */

#define MAX_FSTYPES 32
#define FSTYPE_MAXLEN 32

/* Read /proc/filesystems from LKL and return block filesystem types */
static int get_block_fstypes(char fstypes[][FSTYPE_MAXLEN], int max)
{
	int fd = lkl_sys_open("/proc/filesystems", LKL_O_RDONLY, 0);
	if (fd < 0)
		return 0;

	char buf[2048];
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

int anyfs_mount(int disk_id, unsigned int part, const char* fstype,
		const char* name, uint32_t flags, AnyfsMount* out)
{
	if (!name || !out)
		return -1;

	int auto_detect = (!fstype || strcmp(fstype, "auto") == 0);

	/* Get block device */
	uint32_t dev;
	int ret = lkl_get_virtio_blkdev(disk_id, part, &dev);
	if (ret < 0)
		return ret;

	/* Create device node */
	char dev_str[32];
	snprintf(dev_str, sizeof(dev_str), "/dev/%08x", dev);
	lkl_sys_access("/dev", 0) < 0 && lkl_sys_mkdir("/dev", 0700);
	lkl_sys_mknod(dev_str, LKL_S_IFBLK | 0600, dev);

	/* Create mount point */
	lkl_sys_mkdir("/lklmnt", 0755);
	char mnt[64];
	snprintf(mnt, sizeof(mnt), "/lklmnt/%s", name);
	lkl_sys_mkdir(mnt, 0755);

	int mount_flags = 0;
	if (flags & ANYFS_MOUNT_RDONLY)
		mount_flags |= LKL_MS_RDONLY;

	if (!auto_detect) {
		/* Explicit filesystem type */
		const char* opts = NULL;
		if ((flags & ANYFS_MOUNT_RDONLY) &&
		    (strcmp(fstype, "xfs") == 0 ||
		     strcmp(fstype, "btrfs") == 0))
			opts = "norecovery";

		ret = lkl_sys_mount(dev_str, mnt, (char*)fstype, mount_flags,
				    (char*)opts);
		if (ret < 0) {
			lkl_sys_rmdir(mnt);
			lkl_sys_unlink(dev_str);
			return ret;
		}
		strncpy(out->mount_point, mnt, sizeof(out->mount_point) - 1);
		out->mount_point[sizeof(out->mount_point) - 1] = '\0';
		strncpy(out->fstype, fstype, sizeof(out->fstype) - 1);
		out->fstype[sizeof(out->fstype) - 1] = '\0';
		return 0;
	}

	/* Auto-detect: ensure procfs is mounted for /proc/filesystems */
	if (lkl_sys_access("/proc/filesystems", 0) < 0) {
		lkl_sys_mkdir("/proc", 0555);
		lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
	}

	/* Get list of block filesystems from kernel */
	char fstypes[MAX_FSTYPES][FSTYPE_MAXLEN];
	int nfs = get_block_fstypes(fstypes, MAX_FSTYPES);

	/* Try each filesystem type */
	for (int i = 0; i < nfs; i++) {
		const char* opts = NULL;
		/* xfs/btrfs need norecovery for read-only images */
		if ((flags & ANYFS_MOUNT_RDONLY) &&
		    (strcmp(fstypes[i], "xfs") == 0 ||
		     strcmp(fstypes[i], "btrfs") == 0))
			opts = "norecovery";

		ret = lkl_sys_mount(dev_str, mnt, fstypes[i], mount_flags,
				    (char*)opts);
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

	/* All failed - clean up */
	lkl_sys_rmdir(mnt);
	lkl_sys_unlink(dev_str);
	return -1;
}

int anyfs_umount(const char* name)
{
	if (!name)
		return -1;

	char mnt[64];
	snprintf(mnt, sizeof(mnt), "/lklmnt/%s", name);

	int ret = lkl_sys_umount(mnt, 0);
	if (ret < 0)
		return ret;

	lkl_sys_rmdir(mnt);
	return 0;
}

int anyfs_remount_ro(const char* name)
{
	if (!name)
		return -1;

	char mnt[64];
	snprintf(mnt, sizeof(mnt), "/lklmnt/%s", name);

	return lkl_sys_mount("", mnt, NULL, LKL_MS_REMOUNT | LKL_MS_RDONLY,
			     NULL);
}
