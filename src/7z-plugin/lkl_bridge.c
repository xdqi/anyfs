/* lkl_bridge.c — C implementation of LKL bridge (compiled as C, not C++) */

#include "lkl_bridge.h"
#include <anyfs.h>
#include <lkl.h>
#include <lkl_host.h>
#include <stdlib.h>
#include <string.h>

#ifndef DT_DIR
#define DT_DIR 4
#endif

static int g_kernel_started = 0;

int lklb_kernel_init(uint32_t mem_mb)
{
	if (g_kernel_started)
		return 0;

	AnyfsKernelOpts opts = {0};
	opts.mem_mb = mem_mb ? mem_mb : 64;
	opts.loglevel = 0;

	int ret = anyfs_kernel_init(&opts);
	if (ret == 0)
		g_kernel_started = 1;
	return ret;
}

int lklb_disk_add(const char* image_path, int readonly)
{
	uint32_t flags = ANYFS_BACKEND_QEMU;
	if (readonly)
		flags |= ANYFS_DISK_READONLY;
	return anyfs_disk_add(image_path, flags);
}

int lklb_disk_remove(int disk_id)
{
	return anyfs_disk_remove(disk_id);
}

int lklb_mount(int disk_id, unsigned int part, const char* name,
	       char* mount_point_out, size_t mp_size, char* fstype_out,
	       size_t fs_size)
{
	AnyfsMount mount = {{0}};
	int ret =
	    anyfs_mount(disk_id, part, NULL, name, ANYFS_MOUNT_RDONLY, &mount);
	if (ret < 0) {
		/* Try each partition */
		int nparts = anyfs_disk_partitions(disk_id);
		for (int p = 1; p <= nparts && ret < 0; p++) {
			ret = anyfs_mount(disk_id, p, NULL, name,
					  ANYFS_MOUNT_RDONLY, &mount);
		}
	}
	if (ret == 0) {
		if (mount_point_out)
			strncpy(mount_point_out, mount.mount_point,
				mp_size - 1);
		if (fstype_out)
			strncpy(fstype_out, mount.fstype, fs_size - 1);
	}
	return ret;
}

int lklb_umount(const char* name)
{
	return anyfs_umount(name);
}

int lklb_partitions(int disk_id)
{
	return anyfs_disk_partitions(disk_id);
}

/* ── Directory operations ──────────────────────────────────────── */

struct lklb_dir {
	struct lkl_dir* dir;
};

lklb_dir_t* lklb_opendir(const char* path)
{
	int err;
	struct lkl_dir* d = lkl_opendir(path, &err);
	if (!d)
		return NULL;

	lklb_dir_t* ctx = (lklb_dir_t*)malloc(sizeof(lklb_dir_t));
	if (!ctx) {
		lkl_closedir(d);
		return NULL;
	}
	ctx->dir = d;
	return ctx;
}

int lklb_readdir(lklb_dir_t* dir, LklbDirEntry* entry)
{
	if (!dir || !entry)
		return 0;

	struct lkl_linux_dirent64* de = lkl_readdir(dir->dir);
	if (!de)
		return 0;

	strncpy(entry->name, de->d_name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->is_dir = (de->d_type == DT_DIR);
	entry->size = 0;
	entry->mtime = 0;
	return 1;
}

void lklb_closedir(lklb_dir_t* dir)
{
	if (dir) {
		lkl_closedir(dir->dir);
		free(dir);
	}
}

/* ── File operations ───────────────────────────────────────────── */

int lklb_stat(const char* path, uint64_t* size, uint64_t* mtime, int* is_dir)
{
	struct lkl_stat st;
	int ret = lkl_sys_lstat(path, &st);
	if (ret < 0)
		return ret;
	if (size)
		*size = (uint64_t)st.st_size;
	if (mtime)
		*mtime = (uint64_t)st.lkl_st_mtime;
	if (is_dir)
		*is_dir = ((st.st_mode & LKL_S_IFMT) == LKL_S_IFDIR);
	return 0;
}

int lklb_open(const char* path)
{
	return lkl_sys_open(path, LKL_O_RDONLY, 0);
}

long lklb_read(int fd, void* buf, unsigned long count)
{
	return lkl_sys_read(fd, buf, count);
}

void lklb_close(int fd)
{
	lkl_sys_close(fd);
}
