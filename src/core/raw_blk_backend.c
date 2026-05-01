#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "raw_blk_backend.h"

#include <fcntl.h>
#include <lkl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

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

	struct raw_blk_ctx* ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		close(fd);
		return -1;
	}
	ctx->fd = fd;
	ctx->capacity = (uint64_t)st.st_size;

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
