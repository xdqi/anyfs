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

/* atexit safety net: read /proc/mounts, unmount everything under /lklmnt/,
 * remove all disks, then halt the kernel. */
static void anyfs_atexit_cleanup(void)
{
	if (!g_kernel_started)
		return;

	lkl_sys_sync();

	/* Read /proc/mounts to find all our mount points */
	int fd = lkl_sys_open("/proc/mounts", LKL_O_RDONLY, 0);
	if (fd >= 0) {
		char buf[4096];
		long n;
		while ((n = lkl_sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
			buf[n] = '\0';
			/* Parse lines: "device mountpoint fstype options ..."
			 */
			char* line = buf;
			while (line && *line) {
				char* eol = strchr(line, '\n');
				if (eol)
					*eol = '\0';

				/* Find mount point (second field) */
				char* mp = strchr(line, ' ');
				if (mp) {
					mp++;
					char* mp_end = strchr(mp, ' ');
					if (mp_end)
						*mp_end = '\0';

					if (strncmp(mp, "/lklmnt/", 8) == 0) {
						lkl_sys_umount(mp, 0);
					}
				}

				line = eol ? eol + 1 : NULL;
			}
		}
		lkl_sys_close(fd);
	}

	/* Remove all disk devices */
	for (int i = 0; i < MAX_DISKS; i++) {
		if (g_disks[i].in_use) {
			lkl_disk_remove(g_disks[i].disk);
			g_disks[i].backend->close(&g_disks[i].disk);
			g_disks[i].in_use = 0;
		}
	}

	lkl_sys_halt();
	lkl_cleanup();
	g_kernel_started = 0;
}

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

	/* Work around TLS issues when LKL is loaded as a shared library.
	 * Without this, pthread_getspecific() may fail due to duplicated
	 * __pthread_keys across namespaces (see posix-host.c).
	 * Not applicable to Windows (no RTLD_LOCAL equivalent for DLLs). */
#ifndef _WIN32
	lkl_change_tls_mode();
#endif

	char boot_args[128];
	snprintf(boot_args, sizeof(boot_args), "mem=%uM loglevel=%u", mem_mb,
		 loglevel);
	ret = lkl_start_kernel(boot_args);
	if (ret)
		return -1;

	g_kernel_started = 1;

	/* Mount sysfs early — the multi-partition session layer walks
	 * /sys/block/<vdN>/ to discover partitions. Tolerate EEXIST/EBUSY
	 * if something already mounted it. */
	lkl_sys_mkdir("/sys", 0555);
	{
		long mret = lkl_sys_mount("sysfs", "/sys", "sysfs", 0, NULL);
		(void)mret; /* -EBUSY/-EEXIST are fine */
	}
	/* procfs is mounted lazily by anyfs_mount() when needed; the
	 * session layer also benefits from it being available — mount it
	 * here too so /proc/mounts/filesystems are usable right after
	 * kernel init. */
	lkl_sys_mkdir("/proc", 0555);
	{
		long mret = lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
		(void)mret;
	}

	atexit(anyfs_atexit_cleanup);
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
		const char* opts = NULL;
		if (flags & ANYFS_MOUNT_RDONLY) {
			if (strcmp(fstypes[i], "xfs") == 0 ||
			    strcmp(fstypes[i], "btrfs") == 0)
				opts = "norecovery";
			else if (strcmp(fstypes[i], "ext4") == 0 ||
				 strcmp(fstypes[i], "ext3") == 0)
				opts = "noload";
		}
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

int anyfs_remount_ro(const char* name)
{
	if (!name)
		return -1;

	char mnt[64];
	snprintf(mnt, sizeof(mnt), "/lklmnt/%s", name);

	return lkl_sys_mount("", mnt, NULL, LKL_MS_REMOUNT | LKL_MS_RDONLY,
			     NULL);
}

int anyfs_disk_partitions(int disk_id)
{
	int count = 0;
	uint32_t dev;

	/* Try partition numbers 1..128 until one fails */
	for (unsigned int p = 1; p <= 128; p++) {
		if (lkl_get_virtio_blkdev(disk_id, p, &dev) < 0)
			break;
		count++;
	}
	return count;
}
