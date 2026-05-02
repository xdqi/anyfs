#define _GNU_SOURCE
#include "anyfs_api.h"
#include "anyfs_backend.h"
#include "raw_blk_backend.h"
#ifdef ANYFS_HAS_GIO
#include "gio_blk_backend.h"
#endif
#ifdef ANYFS_HAS_QEMU
#include "qemu_blk_backend.h"
#endif

#include <fcntl.h>
#include <lkl.h>
#include <lkl_host.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Internal structures */

struct AnyfsContext {
	int initialized;
	int disk_id;
	const struct anyfs_backend_ops* backend;
	struct lkl_disk disk;
};

struct AnyfsMount {
	AnyfsContext* ctx;
	uint32_t part_index;
	char mount_point[32];
};

struct AnyfsDir {
	AnyfsMount* mnt;
	struct lkl_dir* lkl_dir;
};

/* Convert LKL error to ANYFS error */
static int32_t lkl_err_to_anyfs(long err)
{
	if (err == 0)
		return ANYFS_OK;
	/* LKL returns negative errno values */
	if (err > 0)
		err = -err;
	switch (err) {
	case -LKL_ENOMEM:
		return ANYFS_ERR_NOMEM;
	case -LKL_EIO:
		return ANYFS_ERR_IO;
	case -LKL_EINVAL:
		return ANYFS_ERR_INVAL;
	case -LKL_ENOENT:
		return ANYFS_ERR_NOENT;
	case -LKL_ENOTDIR:
		return ANYFS_ERR_NOTDIR;
	case -LKL_EBUSY:
		return ANYFS_ERR_BUSY;
	case -LKL_ENOSYS:
		return ANYFS_ERR_NOSYS;
	default:
		return ANYFS_ERR_IO;
	}
}

ANYFS_API int32_t ANYFS_CALL anyfs_init(AnyfsContext** ctx_out,
					const AnyfsInitOpts* opts)
{
	if (!ctx_out)
		return ANYFS_ERR_INVAL;

	uint32_t mem_mb = 64;
	uint32_t loglevel = 0;
	if (opts) {
		if (opts->mem_mb)
			mem_mb = opts->mem_mb;
		loglevel = opts->loglevel;
	}

	/* Register built-in backends */
	anyfs_register_backend(&raw_backend_ops);
#ifdef ANYFS_HAS_GIO
	anyfs_register_backend(&gio_backend_ops);
#endif
#ifdef ANYFS_HAS_QEMU
	anyfs_register_backend(&qemu_backend_ops);
#endif

	AnyfsContext* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return ANYFS_ERR_NOMEM;

	int ret = lkl_init(&lkl_host_ops);
	if (ret) {
		free(ctx);
		return ANYFS_ERR_IO;
	}

	char boot_args[128];
	snprintf(boot_args, sizeof(boot_args), "mem=%uM loglevel=%u", mem_mb,
		 loglevel);
	ret = lkl_start_kernel(boot_args);
	if (ret) {
		free(ctx);
		return ANYFS_ERR_IO;
	}

	ctx->initialized = 1;
	ctx->disk_id = -1;
	*ctx_out = ctx;
	return ANYFS_OK;
}

ANYFS_API void ANYFS_CALL anyfs_destroy(AnyfsContext* ctx)
{
	if (!ctx)
		return;
	if (ctx->disk_id >= 0) {
		lkl_disk_remove(ctx->disk);
		ctx->backend->close(&ctx->disk);
	}
	if (ctx->initialized)
		lkl_sys_halt();
	free(ctx);
}

ANYFS_API int32_t ANYFS_CALL anyfs_open_image(AnyfsContext* ctx,
					      const char* image_path,
					      uint32_t flags)
{
	if (!ctx || !image_path)
		return ANYFS_ERR_INVAL;
	if (ctx->disk_id >= 0)
		return ANYFS_ERR_BUSY;

	int readonly = (flags & ANYFS_OPEN_READONLY) ? 1 : 0;
	int ret;

	/* Select backend */
	const struct anyfs_backend_ops* ops = &raw_backend_ops;
#ifdef ANYFS_HAS_QEMU
	if (flags & ANYFS_OPEN_QEMU)
		ops = &qemu_backend_ops;
	else
#endif
#ifdef ANYFS_HAS_GIO
	    if (flags & ANYFS_OPEN_GIO)
		ops = &gio_backend_ops;
	else
#endif
	{ /* raw */
	}

	ret = ops->open(image_path, readonly, &ctx->disk);
	if (ret < 0)
		return ANYFS_ERR_IO;

	ctx->backend = ops;
	ctx->disk_id = lkl_disk_add(&ctx->disk);
	if (ctx->disk_id < 0) {
		ops->close(&ctx->disk);
		return ANYFS_ERR_IO;
	}
	return ANYFS_OK;
}

ANYFS_API int32_t ANYFS_CALL anyfs_mount(AnyfsContext* ctx, const char* fs_type,
					 uint32_t part_index,
					 AnyfsMount** mount_out)
{
	if (!ctx || !fs_type || !mount_out)
		return ANYFS_ERR_INVAL;
	if (ctx->disk_id < 0)
		return ANYFS_ERR_INVAL;

	AnyfsMount* mnt = calloc(1, sizeof(*mnt));
	if (!mnt)
		return ANYFS_ERR_NOMEM;

	mnt->ctx = ctx;
	mnt->part_index = part_index;

	/* xfs needs norecovery for read-only mount */
	const char* opts = NULL;
	if (strcmp(fs_type, "xfs") == 0)
		opts = "norecovery";

	long ret =
	    lkl_mount_dev(ctx->disk_id, part_index, fs_type, LKL_MS_RDONLY,
			  opts, mnt->mount_point, sizeof(mnt->mount_point));
	if (ret) {
		free(mnt);
		return lkl_err_to_anyfs(ret);
	}

	*mount_out = mnt;
	return ANYFS_OK;
}

ANYFS_API int32_t ANYFS_CALL anyfs_umount(AnyfsMount* mnt)
{
	if (!mnt)
		return ANYFS_ERR_INVAL;

	long ret = lkl_umount_dev(mnt->ctx->disk_id, mnt->part_index, 0, 1000);
	free(mnt);
	return lkl_err_to_anyfs(ret);
}

ANYFS_API anyfs_fd_t ANYFS_CALL anyfs_open(AnyfsMount* mnt, const char* path,
					   uint32_t flags)
{
	(void)flags;
	if (!mnt || !path)
		return -ANYFS_ERR_INVAL;

	/* Build full path: mount_point + "/" + path */
	char fullpath[512];
	int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", mnt->mount_point,
			 path);
	if (n < 0 || (size_t)n >= sizeof(fullpath))
		return -ANYFS_ERR_INVAL;

	long fd = lkl_sys_open(fullpath, LKL_O_RDONLY, 0);
	if (fd < 0)
		return (anyfs_fd_t)fd; /* negative errno */
	return (anyfs_fd_t)fd;
}

ANYFS_API int64_t ANYFS_CALL anyfs_read(AnyfsMount* mnt, anyfs_fd_t fd,
					void* buf, uint64_t count)
{
	if (!mnt || !buf)
		return -ANYFS_ERR_INVAL;

	long ret = lkl_sys_read((int)fd, buf, (int)count);
	return (int64_t)ret;
}

ANYFS_API int32_t ANYFS_CALL anyfs_close(AnyfsMount* mnt, anyfs_fd_t fd)
{
	if (!mnt)
		return ANYFS_ERR_INVAL;
	long ret = lkl_sys_close((int)fd);
	return lkl_err_to_anyfs(ret);
}

ANYFS_API AnyfsDir* ANYFS_CALL anyfs_opendir(AnyfsMount* mnt, const char* path)
{
	if (!mnt || !path)
		return NULL;

	char fullpath[512];
	int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", mnt->mount_point,
			 path);
	if (n < 0 || (size_t)n >= sizeof(fullpath))
		return NULL;

	int err;
	struct lkl_dir* d = lkl_opendir(fullpath, &err);
	if (!d)
		return NULL;

	AnyfsDir* dir = calloc(1, sizeof(*dir));
	if (!dir) {
		lkl_closedir(d);
		return NULL;
	}
	dir->mnt = mnt;
	dir->lkl_dir = d;
	return dir;
}

ANYFS_API int32_t ANYFS_CALL anyfs_readdir(AnyfsDir* dir, AnyfsEntry* entry_out)
{
	if (!dir || !entry_out)
		return ANYFS_ERR_INVAL;

	struct lkl_linux_dirent64* de = lkl_readdir(dir->lkl_dir);
	if (!de)
		return ANYFS_ERR_NOENT; /* end of directory */

	memset(entry_out, 0, sizeof(*entry_out));
	entry_out->type = de->d_type;
	entry_out->inode = de->d_ino;
	/* size not available from readdir alone, leave as 0 */
	strncpy(entry_out->name, de->d_name, sizeof(entry_out->name) - 1);
	return ANYFS_OK;
}

ANYFS_API int32_t ANYFS_CALL anyfs_closedir(AnyfsDir* dir)
{
	if (!dir)
		return ANYFS_ERR_INVAL;
	lkl_closedir(dir->lkl_dir);
	free(dir);
	return ANYFS_OK;
}

ANYFS_API int32_t ANYFS_CALL anyfs_statvfs(AnyfsMount* mnt, AnyfsStatvfs* out)
{
	if (!mnt || !out)
		return ANYFS_ERR_INVAL;

	struct lkl_statfs buf;
	long ret = lkl_sys_statfs(mnt->mount_point, &buf);
	if (ret < 0)
		return lkl_err_to_anyfs(ret);

	out->f_bsize = buf.f_bsize;
	out->f_blocks = buf.f_blocks;
	out->f_bfree = buf.f_bfree;
	out->f_bavail = buf.f_bavail;
	out->f_files = buf.f_files;
	out->f_ffree = buf.f_ffree;
	return ANYFS_OK;
}
