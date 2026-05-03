#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "raw_blk_backend.h"

#include <lkl.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

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

int raw_blk_open(const char* path, int readonly, struct lkl_disk* disk_out)
{
	DWORD access = readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
	DWORD share = FILE_SHARE_READ;
	HANDLE hFile = CreateFileA(path, access, share, NULL, OPEN_EXISTING,
				   FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return -1;

	LARGE_INTEGER size;
	if (!GetFileSizeEx(hFile, &size)) {
		CloseHandle(hFile);
		return -1;
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

int raw_blk_open(const char* path, int readonly, struct lkl_disk* disk_out)
{
	int flags = readonly ? O_RDONLY : O_RDWR;
	int fd = open(path, flags);
	if (fd < 0)
		return -1;

	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}

	uint64_t capacity;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &capacity) < 0) {
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
