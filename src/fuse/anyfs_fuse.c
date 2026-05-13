/*
 * anyfs-fuse — FUSE frontend for anyfs-reader using QEMU block backend
 *
 * Mounts disk images (raw, qcow2, vmdk, etc.) via FUSE.
 * Uses anyfs.h API for kernel/disk/mount management.
 * FUSE callbacks adapted from Linux tools/lkl/lklfuse.c.
 *
 * Usage: anyfs-fuse <image> <mountpoint> [options]
 *   -o backend=NAME     block backend: qemu (default), raw
 *   -o fstype=TYPE      filesystem type (default: auto-detect)
 *   -o part=N           partition number (default: 0 = whole disk)
 *   -o ro               mount read-only
 *   -o mem=N            kernel memory in MB (default: 64)
 *   -o loglevel=N       kernel log level 0-7 (default: 0)
 *   -o opts=OPTS        extra mount options
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 35

#include <errno.h>
#include <fuse3/fuse.h>
#include <fuse3/fuse_opt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Platform type compatibility: Linux libfuse3 uses POSIX types (struct stat,
 * off_t, etc.). WinFSP on native Windows uses its own types (struct fuse_stat,
 * fuse_off_t, etc.). On Cygwin these map back to POSIX.
 *
 * Define portable aliases so callback signatures work on both platforms.
 */
#ifdef _WIN32
/*
 * WinFSP defines these as structs (not typedefs), so we need explicit
 * typedefs to use them without the struct keyword.
 *
 * WinFSP's fuse3/fuse_common.h and fuse/fuse_common.h use the same
 * header guard (FUSE_COMMON_H_), so fuse_parse_cmdline from the latter
 * is blocked. We declare it manually.
 */
#include <fcntl.h>
typedef struct fuse_stat fuse_stat;
typedef struct fuse_statvfs fuse_statvfs;
typedef struct fuse_timespec fuse_timespec;

/* O_* flags that exist on Linux but not in MinGW <fcntl.h>.
 * WinFSP never sets these in fi->flags, so defining them to 0 is safe. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_DSYNC
#define O_DSYNC 0
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif

/* fuse_parse_cmdline: not exported by the WinFSP DLL we have.
 * Implement locally — on Windows we only need mountpoint, foreground,
 * and single-threaded flags. -o options were already handled by
 * fuse_opt_parse above. */
static int fuse_parse_cmdline(struct fuse_args* args, char** mountpoint,
			      int* multithreaded, int* foreground)
{
	int i;
	*mountpoint = NULL;
	*multithreaded = 0;
	*foreground = 0;

	for (i = 0; i < args->argc; i++) {
		if (!args->argv[i])
			continue;
		if (strcmp(args->argv[i], "-f") == 0) {
			*foreground = 1;
			args->argv[i] = NULL;
		} else if (strcmp(args->argv[i], "-s") == 0) {
			*multithreaded = 0;
			args->argv[i] = NULL;
		} else if (strcmp(args->argv[i], "-d") == 0) {
			/* debug — ignore, fuse_new handles it */
		} else if (args->argv[i][0] != '-') {
			if (!*mountpoint)
				*mountpoint = strdup(args->argv[i]);
			args->argv[i] = NULL;
		}
	}
	return 0;
}
#else
// Linux: map portable names to POSIX types
#include <fuse3/fuse_lowlevel.h> // provides fuse_parse_cmdline, fuse_cmdline_opts
#include <sys/stat.h>
typedef struct stat fuse_stat;
typedef struct statvfs fuse_statvfs;
#define fuse_off_t off_t
#define fuse_mode_t mode_t
#define fuse_dev_t dev_t
typedef unsigned int fuse_uid_t;
typedef unsigned int fuse_gid_t;
typedef struct timespec fuse_timespec;
#endif

#include "anyfs.h"

/* ── Global state (matching lklfuse.c pattern) ──────────────────── */

static int g_disk_id = -1;
static int g_part;
static char g_mount_point[64]; /* LKL mount path, e.g. /lklmnt/fuse0 */

/* ── Configuration ───────────────────────────────────────────────── */

struct anyfs_fuse_config {
	char* image;
	int part;
	int readonly;
	int mem_mb;
	int loglevel;
	char* fstype;
	char* opts;
	char* backend;
};

static struct anyfs_fuse_config cfg = {
    .part = 0,
    .readonly = 1,
    .mem_mb = 64,
    .loglevel = 0,
};

#define ANYFS_FUSE_OPT(t, p, v) {t, offsetof(struct anyfs_fuse_config, p), v}

enum {
	KEY_HELP,
	KEY_VERSION,
};

static struct fuse_opt anyfs_fuse_opts[] = {
    ANYFS_FUSE_OPT("backend=%s", backend, 0),
    ANYFS_FUSE_OPT("fstype=%s", fstype, 0),
    ANYFS_FUSE_OPT("part=%d", part, 0),
    ANYFS_FUSE_OPT("mem=%d", mem_mb, 0),
    ANYFS_FUSE_OPT("loglevel=%d", loglevel, 0),
    ANYFS_FUSE_OPT("opts=%s", opts, 0),
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-V", KEY_VERSION),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    FUSE_OPT_END};

static void usage(void)
{
	printf(
	    "usage: anyfs-fuse <image> <mountpoint> [options]\n"
	    "\n"
	    "general options:\n"
	    "    -o opt,[opt...]     mount options\n"
	    "    -h   --help         print help\n"
	    "    -V   --version      print version\n"
	    "\n"
	    "anyfs-fuse options:\n"
	    "    -o backend=NAME     block backend: qemu (default), raw\n"
	    "    -o fstype=TYPE      filesystem type (default: auto-detect)\n"
	    "    -o part=N           partition number (default: 0 = whole "
	    "disk)\n"
	    "    -o ro               mount read-only\n"
	    "    -o mem=N            kernel memory in MB (default: 64)\n"
	    "    -o loglevel=N       kernel log level 0-7 (default: 0)\n"
	    "    -o opts=OPTS        extra mount options\n");
}

static int anyfs_fuse_opt_proc(void* data, const char* arg, int key,
			       struct fuse_args* args)
{
	(void)data;

	switch (key) {
	case FUSE_OPT_KEY_OPT:
		if (strcmp(arg, "ro") == 0) {
			cfg.readonly = 1;
			return 1;
		}
		return 1;

	case FUSE_OPT_KEY_NONOPT:
		if (!cfg.image) {
			cfg.image = strdup(arg);
			return 0;
		}
		return 1;

	case KEY_HELP:
		usage();
		args->argv[0] = "";
		fuse_opt_add_arg(args, "-h");
		fuse_main(args->argc, args->argv, NULL, NULL);
		exit(1);

	case KEY_VERSION:
		printf("anyfs-fuse version 0.2\n");
		fuse_opt_add_arg(args, "--version");
		fuse_main(args->argc, args->argv, NULL, NULL);
		exit(0);

	default:
		fprintf(stderr, "internal error\n");
		abort();
	}
}

/* ── FUSE operations (adapted from lklfuse.c) ──────────────────── */

static void xlat_stat(const struct lkl_stat* in, fuse_stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev = in->st_dev;
	st->st_ino = in->st_ino;
	st->st_mode = in->st_mode;
	st->st_nlink = in->st_nlink;
	st->st_uid = in->st_uid;
	st->st_gid = in->st_gid;
	st->st_rdev = in->st_rdev;
	st->st_size = in->st_size;
	st->st_blksize = in->st_blksize;
	st->st_blocks = in->st_blocks;
	st->st_atim.tv_sec = in->lkl_st_atime;
	st->st_atim.tv_nsec = in->st_atime_nsec;
	st->st_mtim.tv_sec = in->lkl_st_mtime;
	st->st_mtim.tv_nsec = in->st_mtime_nsec;
	st->st_ctim.tv_sec = in->lkl_st_ctime;
	st->st_ctim.tv_nsec = in->st_ctime_nsec;
}

/*
 * Path prefix helper: prepend LKL mount point to FUSE paths.
 * This replaces lkl_sys_chroot() which is not available in all LKL builds
 * (e.g. MinGW-exported liblkl.dll does not export all Linux syscalls).
 */
static void lkl_path(char* buf, size_t size, const char* path)
{
	if (path[0] == '/')
		path++;
	int n = snprintf(buf, size, "%s/%s", g_mount_point, path);
	if (n < 0 || (size_t)n >= size)
		buf[size - 1] = '\0';
}

#define LPATH_BUF_SIZE 4096
#define LPATH_DECL char _lpath[LPATH_BUF_SIZE]
#define LPATH(path) (lkl_path(_lpath, sizeof(_lpath), path), _lpath)

static int anyfs_fuse_getattr(const char* path, fuse_stat* st,
			      struct fuse_file_info* fi)
{
	LPATH_DECL;
	struct lkl_stat lkl_stat;
	long ret;

	if (fi)
		ret = lkl_sys_fstat(fi->fh, &lkl_stat);
	else
		ret = lkl_sys_lstat(LPATH(path), &lkl_stat);

	if (!ret)
		xlat_stat(&lkl_stat, st);
	return ret;
}

static int anyfs_fuse_readlink(const char* path, char* buf, size_t len)
{
	LPATH_DECL;
	long ret = lkl_sys_readlink(LPATH(path), buf, len);
	if (ret < 0)
		return ret;
	if ((size_t)ret == len)
		ret = len - 1;
	buf[ret] = 0;
	return 0;
}

static int anyfs_fuse_mknod(const char* path, fuse_mode_t mode, fuse_dev_t dev)
{
	LPATH_DECL;
	return lkl_sys_mknod(LPATH(path), mode, dev);
}

static int anyfs_fuse_mkdir(const char* path, fuse_mode_t mode)
{
	LPATH_DECL;
	return lkl_sys_mkdir(LPATH(path), mode);
}

static int anyfs_fuse_unlink(const char* path)
{
	LPATH_DECL;
	return lkl_sys_unlink(LPATH(path));
}

static int anyfs_fuse_rmdir(const char* path)
{
	LPATH_DECL;
	return lkl_sys_rmdir(LPATH(path));
}

static int anyfs_fuse_symlink(const char* oldname, const char* newname)
{
	/* oldname = symlink target (string, not a path — skip LPATH) */
	char _lpath2[LPATH_BUF_SIZE];
	lkl_path(_lpath2, sizeof(_lpath2), newname);
	return lkl_sys_symlink(oldname, _lpath2);
}

static int anyfs_fuse_rename(const char* oldname, const char* newname,
			     unsigned int flags)
{
	LPATH_DECL;
	char _lpath2[LPATH_BUF_SIZE];
	lkl_path(_lpath2, sizeof(_lpath2), newname);
	return lkl_sys_renameat2(LKL_AT_FDCWD, LPATH(oldname), LKL_AT_FDCWD,
				 _lpath2, flags);
}

static int anyfs_fuse_link(const char* oldname, const char* newname)
{
	LPATH_DECL;
	char _lpath2[LPATH_BUF_SIZE];
	lkl_path(_lpath2, sizeof(_lpath2), newname);
	return lkl_sys_link(LPATH(oldname), _lpath2);
}

static int anyfs_fuse_chmod(const char* path, fuse_mode_t mode,
			    struct fuse_file_info* fi)
{
	LPATH_DECL;
	if (fi)
		return lkl_sys_fchmod(fi->fh, mode);
	return lkl_sys_fchmodat(LKL_AT_FDCWD, LPATH(path), mode);
}

static int anyfs_fuse_chown(const char* path, fuse_uid_t uid, fuse_gid_t gid,
			    struct fuse_file_info* fi)
{
	LPATH_DECL;
	if (fi)
		return lkl_sys_fchown(fi->fh, uid, gid);
	return lkl_sys_fchownat(LKL_AT_FDCWD, LPATH(path), uid, gid,
				LKL_AT_SYMLINK_NOFOLLOW);
}

static int anyfs_fuse_truncate(const char* path, fuse_off_t off,
			       struct fuse_file_info* fi)
{
	LPATH_DECL;
	if (fi)
		return lkl_sys_ftruncate(fi->fh, off);
	return lkl_sys_truncate(LPATH(path), off);
}

static int anyfs_fuse_open3(const char* path, bool create, fuse_mode_t mode,
			    struct fuse_file_info* fi)
{
	LPATH_DECL;
	long ret;
	int flags = 0;

	switch (fi->flags & O_ACCMODE) {
	case O_RDONLY:
		flags = LKL_O_RDONLY;
		break;
	case O_WRONLY:
		flags = LKL_O_WRONLY;
		break;
	case O_RDWR:
		flags = LKL_O_RDWR;
		break;
	default:
		return -EINVAL;
	}

	if (create)
		flags |= LKL_O_CREAT;
	if (fi->flags & O_TRUNC)
		flags |= LKL_O_TRUNC;
	if (fi->flags & O_APPEND)
		flags |= LKL_O_APPEND;
	if (fi->flags & O_NONBLOCK)
		flags |= LKL_O_NONBLOCK;
	if (fi->flags & O_DSYNC)
		flags |= LKL_O_DSYNC;
	if (fi->flags & O_DIRECT)
		flags |= LKL_O_DIRECT;
	if (fi->flags & O_LARGEFILE)
		flags |= LKL_O_LARGEFILE;
	if (fi->flags & O_DIRECTORY)
		flags |= LKL_O_DIRECTORY;
	if (fi->flags & O_NOFOLLOW)
		flags |= LKL_O_NOFOLLOW;
	if (fi->flags & O_NOATIME)
		flags |= LKL_O_NOATIME;
	if (fi->flags & O_CLOEXEC)
		flags |= LKL_O_CLOEXEC;
	if (fi->flags & O_SYNC)
		flags |= LKL_O_SYNC;

	ret = lkl_sys_open(LPATH(path), flags, mode);
	if (ret < 0)
		return ret;

	fi->fh = ret;
	return 0;
}

static int anyfs_fuse_create(const char* path, fuse_mode_t mode,
			     struct fuse_file_info* fi)
{
	return anyfs_fuse_open3(path, true, mode, fi);
}

static int anyfs_fuse_open(const char* path, struct fuse_file_info* fi)
{
	return anyfs_fuse_open3(path, false, 0, fi);
}

static int anyfs_fuse_read(const char* path, char* buf, size_t size,
			   fuse_off_t offset, struct fuse_file_info* fi)
{
	(void)path;
	long ret;
	ssize_t orig_size = size;

	do {
		ret = lkl_sys_pread64(fi->fh, buf, size, offset);
		if (ret <= 0)
			break;
		size -= ret;
		offset += ret;
		buf += ret;
	} while (size > 0);

	return ret < 0 ? ret : orig_size - (ssize_t)size;
}

static int anyfs_fuse_write(const char* path, const char* buf, size_t size,
			    fuse_off_t offset, struct fuse_file_info* fi)
{
	(void)path;
	long ret;
	ssize_t orig_size = size;

	do {
		ret = lkl_sys_pwrite64(fi->fh, buf, size, offset);
		if (ret <= 0)
			break;
		size -= ret;
		offset += ret;
		buf += ret;
	} while (size > 0);

	return ret < 0 ? ret : orig_size - (ssize_t)size;
}

static int anyfs_fuse_statfs(const char* path, fuse_statvfs* stat)
{
	LPATH_DECL;
	struct lkl_statfs lkl_statfs;
	long ret = lkl_sys_statfs(LPATH(path), &lkl_statfs);
	if (ret < 0)
		return ret;

	memset(stat, 0, sizeof(*stat));
	stat->f_bsize = lkl_statfs.f_bsize;
	stat->f_frsize = lkl_statfs.f_frsize;
	stat->f_blocks = lkl_statfs.f_blocks;
	stat->f_bfree = lkl_statfs.f_bfree;
	stat->f_bavail = lkl_statfs.f_bavail;
	stat->f_files = lkl_statfs.f_files;
	stat->f_ffree = lkl_statfs.f_ffree;
	stat->f_favail = lkl_statfs.f_ffree;
	stat->f_fsid = *(unsigned long*)&lkl_statfs.f_fsid.val[0];
	stat->f_flag = lkl_statfs.f_flags;
	stat->f_namemax = lkl_statfs.f_namelen;
	return 0;
}

static int anyfs_fuse_flush(const char* path, struct fuse_file_info* fi)
{
	(void)path;
	(void)fi;
	return 0;
}

static int anyfs_fuse_release(const char* path, struct fuse_file_info* fi)
{
	(void)path;
	return lkl_sys_close(fi->fh);
}

static int anyfs_fuse_fsync(const char* path, int datasync,
			    struct fuse_file_info* fi)
{
	(void)path;
	if (datasync)
		return lkl_sys_fdatasync(fi->fh);
	return lkl_sys_fsync(fi->fh);
}

static int anyfs_fuse_setxattr(const char* path, const char* name,
			       const char* val, size_t size, int flags)
{
	LPATH_DECL;
	return lkl_sys_setxattr(LPATH(path), name, val, size, flags);
}

static int anyfs_fuse_getxattr(const char* path, const char* name, char* val,
			       size_t size)
{
	LPATH_DECL;
	return lkl_sys_getxattr(LPATH(path), name, val, size);
}

static int anyfs_fuse_listxattr(const char* path, char* list, size_t size)
{
	LPATH_DECL;
	return lkl_sys_listxattr(LPATH(path), list, size);
}

static int anyfs_fuse_removexattr(const char* path, const char* name)
{
	LPATH_DECL;
	return lkl_sys_removexattr(LPATH(path), name);
}

static int anyfs_fuse_opendir(const char* path, struct fuse_file_info* fi)
{
	LPATH_DECL;
	int err;
	struct lkl_dir* dir = lkl_opendir(LPATH(path), &err);
	if (!dir)
		return err;
	fi->fh = (uintptr_t)dir;
	return 0;
}

static int anyfs_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t fill,
			      fuse_off_t off, struct fuse_file_info* fi,
			      enum fuse_readdir_flags flags)
{
	(void)path;
	(void)off;
	(void)flags;
	struct lkl_dir* dir = (struct lkl_dir*)(uintptr_t)fi->fh;
	struct lkl_linux_dirent64* de;

	while ((de = lkl_readdir(dir))) {
		fuse_stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (fill(buf, de->d_name, &st, 0, (enum fuse_fill_dir_flags)0))
			break;
	}

	if (!de)
		return lkl_errdir(dir);
	return 0;
}

static int anyfs_fuse_releasedir(const char* path, struct fuse_file_info* fi)
{
	(void)path;
	return lkl_closedir((struct lkl_dir*)(uintptr_t)fi->fh);
}

static int anyfs_fuse_fsyncdir(const char* path, int datasync,
			       struct fuse_file_info* fi)
{
	(void)path;
	struct lkl_dir* dir = (struct lkl_dir*)(uintptr_t)fi->fh;
	int fd = lkl_dirfd(dir);
	if (datasync)
		return lkl_sys_fdatasync(fd);
	return lkl_sys_fsync(fd);
}

static int anyfs_fuse_access(const char* path, int mode)
{
	LPATH_DECL;
	return lkl_sys_access(LPATH(path), mode);
}

static int anyfs_fuse_utimens(const char* path, const fuse_timespec tv[2],
			      struct fuse_file_info* fi)
{
	LPATH_DECL;
	struct __lkl__kernel_timespec ts[2] = {
	    {.tv_sec = tv[0].tv_sec, .tv_nsec = tv[0].tv_nsec},
	    {.tv_sec = tv[1].tv_sec, .tv_nsec = tv[1].tv_nsec},
	};

	if (fi)
		return lkl_sys_utimensat(fi->fh, NULL, ts, 0);
	return lkl_sys_utimensat(-1, LPATH(path), ts, LKL_AT_SYMLINK_NOFOLLOW);
}

static int anyfs_fuse_fallocate(const char* path, int mode, fuse_off_t offset,
				fuse_off_t len, struct fuse_file_info* fi)
{
	(void)path;
	return lkl_sys_fallocate(fi->fh, mode, offset, len);
}

static ssize_t
anyfs_fuse_copy_file_range(const char* path_in, struct fuse_file_info* fi_in,
			   fuse_off_t off_in, const char* path_out,
			   struct fuse_file_info* fi_out, fuse_off_t off_out,
			   size_t len, int flags)
{
	(void)path_in;
	(void)path_out;
	lkl_loff_t loff_in = off_in, loff_out = off_out;
	return lkl_sys_copy_file_range(fi_in->fh, &loff_in, fi_out->fh,
				       &loff_out, len, flags);
}

static fuse_off_t anyfs_fuse_lseek(const char* path, fuse_off_t off, int whence,
				   struct fuse_file_info* fi)
{
	(void)path;
	return lkl_sys_lseek(fi->fh, off, whence);
}

static void* anyfs_fuse_init(struct fuse_conn_info* conn,
			     struct fuse_config* cfg2)
{
	(void)conn;
	cfg2->nullpath_ok = 1;
	cfg2->entry_timeout = 0;
	cfg2->attr_timeout = 0;
	cfg2->negative_timeout = 0;
	cfg2->use_ino = 1;
	return NULL;
}

static const struct fuse_operations anyfs_fuse_ops = {
    .init = anyfs_fuse_init,
    .getattr = anyfs_fuse_getattr,
    .readlink = anyfs_fuse_readlink,
    .mknod = anyfs_fuse_mknod,
    .mkdir = anyfs_fuse_mkdir,
    .unlink = anyfs_fuse_unlink,
    .rmdir = anyfs_fuse_rmdir,
    .symlink = anyfs_fuse_symlink,
    .rename = anyfs_fuse_rename,
    .link = anyfs_fuse_link,
    .chmod = anyfs_fuse_chmod,
    .chown = anyfs_fuse_chown,
    .truncate = anyfs_fuse_truncate,
    .open = anyfs_fuse_open,
    .read = anyfs_fuse_read,
    .write = anyfs_fuse_write,
    .statfs = anyfs_fuse_statfs,
    .flush = anyfs_fuse_flush,
    .release = anyfs_fuse_release,
    .fsync = anyfs_fuse_fsync,
    .setxattr = anyfs_fuse_setxattr,
    .getxattr = anyfs_fuse_getxattr,
    .listxattr = anyfs_fuse_listxattr,
    .removexattr = anyfs_fuse_removexattr,
    .opendir = anyfs_fuse_opendir,
    .readdir = anyfs_fuse_readdir,
    .releasedir = anyfs_fuse_releasedir,
    .fsyncdir = anyfs_fuse_fsyncdir,
    .access = anyfs_fuse_access,
    .create = anyfs_fuse_create,
    .utimens = anyfs_fuse_utimens,
    .fallocate = anyfs_fuse_fallocate,
#ifndef _WIN32
    .copy_file_range = anyfs_fuse_copy_file_range,
    .lseek = anyfs_fuse_lseek,
#endif
};

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse* fuse = NULL;
	char* cli_mountpoint = NULL;
	int cli_foreground = 0;
	int cli_singlethread = 0;
	int ret = 1;
	int kernel_started = 0;
	char mount_name[32];

	if (fuse_opt_parse(&args, &cfg, anyfs_fuse_opts, anyfs_fuse_opt_proc))
		return 1;

	if (!cfg.image) {
		fprintf(stderr, "error: no disk image specified\n");
		fprintf(stderr,
			"usage: anyfs-fuse <image> <mountpoint> [options]\n");
		goto out;
	}

	/*
	 * fuse_parse_cmdline signature differs between Linux libfuse3 and
	 * WinFSP. Linux:  fuse_parse_cmdline(&args, struct fuse_cmdline_opts
	 * *opts) WinFSP: fuse_parse_cmdline(&args, &mountpoint, &multithreaded,
	 * &foreground)
	 *
	 * Normalize into common local variables for the rest of main().
	 */
#ifdef _WIN32
	{
		int multithreaded = 0;
		if (fuse_parse_cmdline(&args, &cli_mountpoint, &multithreaded,
				       &cli_foreground))
			goto out;
		cli_singlethread = !multithreaded;
	}
#else
	{
		struct fuse_cmdline_opts cli_opts;
		memset(&cli_opts, 0, sizeof(cli_opts));
		if (fuse_parse_cmdline(&args, &cli_opts))
			goto out;
		cli_mountpoint = cli_opts.mountpoint;
		cli_foreground = cli_opts.foreground;
		cli_singlethread = cli_opts.singlethread;
	}
#endif

	if (!cli_mountpoint) {
		fprintf(stderr, "error: no mount point specified\n");
		goto out;
	}

	/*
	 * Init order:
	 *   1. FUSE setup (fuse_new, signal_handlers, mount, daemonize)
	 *   2. LKL kernel start  (AFTER fork — threads in child only)
	 *   3. Disk add + mount in LKL
	 *   4. Store LKL mount point for path prefix translation in callbacks
	 *   5. FUSE event loop
	 *
	 * The critical constraint: lkl_start_kernel() creates internal threads
	 * that use setjmp/longjmp for cooperative scheduling. These threads do
	 * NOT survive fork(). So everything that creates LKL threads must be
	 * AFTER fuse_daemonize().
	 *
	 * On Windows: no fork(), fuse_daemonize() is a no-op.
	 */

	/* ── Step 1: FUSE setup (before LKL threads exist) ── */
	fuse = fuse_new(&args, &anyfs_fuse_ops, sizeof(anyfs_fuse_ops), NULL);
	if (!fuse) {
		ret = 1;
		goto out;
	}

	ret = fuse_set_signal_handlers(fuse_get_session(fuse));
	if (ret < 0)
		goto out_fuse_destroy;

	ret = fuse_mount(fuse, cli_mountpoint);
	if (ret < 0)
		goto out_remove_signals;

	fuse_opt_free_args(&args);

	ret = fuse_daemonize(cli_foreground);
	if (ret < 0)
		goto out_fuse_unmount;

	/* ── Step 2: LKL kernel start (after fork, in child) ── */
	{
		AnyfsKernelOpts kopts = {
		    .mem_mb = cfg.mem_mb,
		    .loglevel = cfg.loglevel,
		};

		ret = anyfs_kernel_init(&kopts);
		if (ret < 0) {
			fprintf(stderr, "anyfs_kernel_init failed: %d\n", ret);
			goto out_fuse_unmount;
		}
		kernel_started = 1;
	}

	/* ── Step 3: Add disk + mount ── */
	{
		uint32_t disk_flags = ANYFS_BACKEND_QEMU;
		if (!cfg.backend || strcmp(cfg.backend, "qemu") == 0) {
			disk_flags = ANYFS_BACKEND_QEMU;
		} else if (strcmp(cfg.backend, "raw") == 0) {
			disk_flags = ANYFS_BACKEND_RAW;
		} else {
			fprintf(stderr, "unknown backend '%s', using qemu\n",
				cfg.backend);
			disk_flags = ANYFS_BACKEND_QEMU;
		}

		if (cfg.readonly)
			disk_flags |= ANYFS_DISK_READONLY;

		g_disk_id = anyfs_disk_add(cfg.image, disk_flags);
		if (g_disk_id < 0) {
			fprintf(stderr, "anyfs_disk_add(%s) failed\n",
				cfg.image);
			ret = 1;
			goto out_kernel_halt;
		}

		g_part = cfg.part;
		snprintf(mount_name, sizeof(mount_name), "fuse%d", g_disk_id);
	}

	/* ── Step 4: Mount in LKL (g_mount_point used by LPATH macro) ── */
	{
		uint32_t mount_flags = cfg.readonly ? ANYFS_MOUNT_RDONLY : 0;
		AnyfsMount amnt;

		ret = anyfs_mount(g_disk_id, cfg.part, cfg.fstype, mount_name,
				  mount_flags, &amnt);
		if (ret < 0) {
			fprintf(stderr, "anyfs_mount(part=%d) failed: %d\n",
				cfg.part, ret);
			goto out_disk_remove;
		}

		snprintf(g_mount_point, sizeof(g_mount_point), "%s",
			 amnt.mount_point);

		const char* backend_name =
		    (!cfg.backend || strcmp(cfg.backend, "qemu") == 0) ? "qemu"
								       : "raw";
		fprintf(
		    stderr,
		    "anyfs-fuse: %s mounted at %s (fstype: %s, backend: %s)\n",
		    cfg.image, cli_mountpoint, amnt.fstype, backend_name);
	}

	/* ── Step 5: FUSE event loop ── */
	if (!cli_singlethread)
		fprintf(stderr, "warning: multithreaded mode not supported, "
				"forcing single-threaded\n");

	ret = fuse_loop(fuse);
	if (ret < 0)
		fprintf(stderr, "fuse_loop error: %d\n", ret);

	/* ── Cleanup ── */
	lkl_sys_sync();
	anyfs_umount(mount_name);

out_disk_remove:
	if (g_disk_id >= 0) {
		anyfs_disk_remove(g_disk_id);
		g_disk_id = -1;
	}

out_kernel_halt:
	if (kernel_started)
		anyfs_kernel_halt();

out_fuse_unmount:
	fuse_unmount(fuse);

out_remove_signals:
	fuse_remove_signal_handlers(fuse_get_session(fuse));

out_fuse_destroy:
	fuse_destroy(fuse);

out:
	free(cli_mountpoint);
	free(cfg.image);
	free(cfg.backend);
	free(cfg.fstype);
	free(cfg.opts);

	return ret;
}
