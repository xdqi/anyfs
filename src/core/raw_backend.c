#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "raw_backend.h"
#include "anyfs.h"
#include "anyfs_u8_win.h"

#include <lkl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

struct raw_blk_ctx {
	HANDLE hFile;
	uint64_t capacity;
};

static int raw_get_capacity(struct lkl_disk disk, unsigned long long* res)
{
	struct raw_blk_ctx* ctx = disk.handle;
	*res = ctx->capacity;
	return 0;
}

static int raw_request(struct lkl_disk disk, struct lkl_blk_req* req)
{
	struct raw_blk_ctx* ctx = disk.handle;
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)req->sector * 512;

	for (int i = 0; i < req->count; i++) {
		DWORD done;
		OVERLAPPED ov = {0};
		ov.Offset = li.LowPart;
		ov.OffsetHigh = li.HighPart;

		switch (req->type) {
		case LKL_DEV_BLK_TYPE_READ:
			if (!ReadFile(ctx->hFile, req->buf[i].iov_base,
				      (DWORD)req->buf[i].iov_len, &done, &ov))
				return LKL_DEV_BLK_STATUS_IOERR;
			break;
		case LKL_DEV_BLK_TYPE_WRITE:
			if (!WriteFile(ctx->hFile, req->buf[i].iov_base,
				       (DWORD)req->buf[i].iov_len, &done, &ov))
				return LKL_DEV_BLK_STATUS_IOERR;
			break;
		case LKL_DEV_BLK_TYPE_FLUSH:
		case LKL_DEV_BLK_TYPE_FLUSH_OUT:
			FlushFileBuffers(ctx->hFile);
			return LKL_DEV_BLK_STATUS_OK;
		default:
			return LKL_DEV_BLK_STATUS_UNSUP;
		}
		li.QuadPart += req->buf[i].iov_len;
	}
	return LKL_DEV_BLK_STATUS_OK;
}

static struct lkl_dev_blk_ops raw_ops = {
    .get_capacity = raw_get_capacity,
    .request = raw_request,
};

int raw_blk_open(const char* path, uint32_t flags, struct lkl_disk* disk_out)
{
	bool readonly = flags & ANYFS_SESSION_READONLY;
	DWORD access = readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
	/* A disk (\\.\PhysicalDriveN) is open in the system already, with
	 * write sharing: opening it without FILE_SHARE_WRITE fails with a
	 * sharing violation. */
	bool device = strncmp(path, "\\\\.\\", 4) == 0;
	DWORD share = device ? (FILE_SHARE_READ | FILE_SHARE_WRITE)
			     : FILE_SHARE_READ;
	HANDLE hFile = anyfs_u8_create_file(path, access, share, OPEN_EXISTING,
					    FILE_ATTRIBUTE_NORMAL);
	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		anyfs_set_last_error("cannot open %s: %s (Windows error %lu)",
				     path,
				     e == ERROR_ACCESS_DENIED ? "Access is denied"
							      : "open failed",
				     (unsigned long)e);
		return -1;
	}

	/* GetFileSizeEx works for files only; a disk reports its length
	 * through IOCTL_DISK_GET_LENGTH_INFO. */
	LARGE_INTEGER size;
	if (!GetFileSizeEx(hFile, &size)) {
		GET_LENGTH_INFORMATION len;
		DWORD got;
		if (!DeviceIoControl(hFile, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
				     &len, sizeof(len), &got, NULL)) {
			anyfs_set_last_error("cannot size %s (Windows error %lu)",
					     path, (unsigned long)GetLastError());
			CloseHandle(hFile);
			return -1;
		}
		size = len.Length;
	}

	struct raw_blk_ctx* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		CloseHandle(hFile);
		return -1;
	}
	ctx->hFile = hFile;
	ctx->capacity = (uint64_t)size.QuadPart;

	memset(disk_out, 0, sizeof(*disk_out));
	disk_out->handle = ctx;
	disk_out->ops = &raw_ops;
	return 0;
}

void raw_blk_destroy(struct lkl_disk* disk)
{
	struct raw_blk_ctx* ctx = disk->handle;
	if (ctx) {
		CloseHandle(ctx->hFile);
		free(ctx);
		disk->handle = NULL;
	}
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* BLKGETSIZE64 ioctl number (avoid including linux/fs.h which conflicts with
 * LKL) */
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

#ifdef __APPLE__
/* macOS sizes a disk as block count x block size. The raw node /dev/rdiskN
 * is a character device. The ioctl numbers are XNU's <sys/disk.h>, which the
 * zig toolchain doesn't ship. */
#define IS_DISK_NODE(m) (S_ISBLK(m) || S_ISCHR(m))
#ifndef DKIOCGETBLOCKSIZE
#define DKIOCGETBLOCKSIZE _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)
#endif

static int blkdev_capacity(int fd, uint64_t* capacity)
{
	uint32_t bsize;
	uint64_t count;

	if (ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) < 0 ||
	    ioctl(fd, DKIOCGETBLOCKCOUNT, &count) < 0)
		return -1;
	*capacity = count * bsize;
	return 0;
}
#else
#define IS_DISK_NODE(m) S_ISBLK(m)

static int blkdev_capacity(int fd, uint64_t* capacity)
{
	return ioctl(fd, BLKGETSIZE64, capacity);
}
#endif

struct raw_blk_ctx {
	int fd;
	uint64_t capacity;
};

static int raw_get_capacity(struct lkl_disk disk, unsigned long long* res)
{
	struct raw_blk_ctx* ctx = disk.handle;
	*res = ctx->capacity;
	return 0;
}

static int raw_request(struct lkl_disk disk, struct lkl_blk_req* req)
{
	struct raw_blk_ctx* ctx = disk.handle;
	off_t offset = (off_t)req->sector * 512;

	for (int i = 0; i < req->count; i++) {
		ssize_t ret;
		switch (req->type) {
		case LKL_DEV_BLK_TYPE_READ:
			ret = pread(ctx->fd, req->buf[i].iov_base,
				    req->buf[i].iov_len, offset);
			if (ret < 0)
				return LKL_DEV_BLK_STATUS_IOERR;
			break;
		case LKL_DEV_BLK_TYPE_WRITE:
			ret = pwrite(ctx->fd, req->buf[i].iov_base,
				     req->buf[i].iov_len, offset);
			if (ret < 0)
				return LKL_DEV_BLK_STATUS_IOERR;
			break;
		case LKL_DEV_BLK_TYPE_FLUSH:
		case LKL_DEV_BLK_TYPE_FLUSH_OUT:
			if (fsync(ctx->fd) < 0)
				return LKL_DEV_BLK_STATUS_IOERR;
			return LKL_DEV_BLK_STATUS_OK;
		default:
			return LKL_DEV_BLK_STATUS_UNSUP;
		}
		offset += req->buf[i].iov_len;
	}
	return LKL_DEV_BLK_STATUS_OK;
}

static struct lkl_dev_blk_ops raw_ops = {
    .get_capacity = raw_get_capacity,
    .request = raw_request,
};

int raw_blk_open(const char* path, uint32_t flags, struct lkl_disk* disk_out)
{
	bool readonly = flags & ANYFS_SESSION_READONLY;
	int oflags = readonly ? O_RDONLY : O_RDWR;
	int fd = open(path, oflags);
	if (fd < 0) {
		anyfs_set_last_error("cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		anyfs_set_last_error("cannot stat %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}

	uint64_t capacity;
	if (IS_DISK_NODE(st.st_mode)) {
		if (blkdev_capacity(fd, &capacity) < 0) {
			anyfs_set_last_error("cannot size %s: %s", path,
					     strerror(errno));
			close(fd);
			return -1;
		}
	} else {
		capacity = (uint64_t)st.st_size;
	}

	struct raw_blk_ctx* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		close(fd);
		return -1;
	}
	ctx->fd = fd;
	ctx->capacity = capacity;

	memset(disk_out, 0, sizeof(*disk_out));
	disk_out->handle = ctx;
	disk_out->ops = &raw_ops;
	return 0;
}

void raw_blk_destroy(struct lkl_disk* disk)
{
	struct raw_blk_ctx* ctx = disk->handle;
	if (ctx) {
		close(ctx->fd);
		free(ctx);
		disk->handle = NULL;
	}
}

#endif /* _WIN32 */

const struct anyfs_backend_ops raw_backend_ops = {
    .name = "raw",
    .open = raw_blk_open,
    .close = raw_blk_destroy,
};
