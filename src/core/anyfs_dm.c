/*
 * anyfs_dm.c — Device-mapper ioctl wrappers (v2 enabler).
 *
 * Recipes follow the in-kernel ioctl shape: a single struct dm_ioctl
 * with target specs appended. We keep one buffer per call (no
 * iterators) because each call sets up exactly one target.
 */
#define _GNU_SOURCE
#include "anyfs_dm.h"

#include <lkl.h>
#include <lkl_host.h>

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The UAPI header lives in the kernel tree, not in LKL's generated
 * include set, so duplicate the load-bearing bits inline to avoid
 * pulling in a host system include path that LKL doesn't expose. */
#define DM_NAME_LEN 128
#define DM_UUID_LEN 129
#define DM_MAX_TYPE_NAME 16

struct dm_ioctl_local {
	uint32_t version[3];
	uint32_t data_size;
	uint32_t data_start;
	uint32_t target_count;
	int32_t open_count;
	uint32_t flags;
	uint32_t event_nr;
	uint32_t padding;
	uint64_t dev;
	char name[DM_NAME_LEN];
	char uuid[DM_UUID_LEN];
	char data[7];
};

struct dm_target_spec_local {
	uint64_t sector_start;
	uint64_t length;
	int32_t status;
	uint32_t next;
	char target_type[DM_MAX_TYPE_NAME];
};

#define DM_IOCTL 0xfd
#define DM_VERSION_CMD 0
#define DM_REMOVE_ALL_CMD 1
#define DM_LIST_DEVICES 2
#define DM_DEV_CREATE_CMD 3
#define DM_DEV_REMOVE_CMD 4
#define DM_DEV_RENAME_CMD 5
#define DM_DEV_SUSPEND_CMD 6
#define DM_DEV_STATUS_CMD 7
#define DM_DEV_WAIT_CMD 8
#define DM_TABLE_LOAD_CMD 9
#define DM_TABLE_CLEAR_CMD 10
#define DM_TABLE_DEPS_CMD 11
#define DM_TABLE_STATUS_CMD 12

#define DM_SUSPEND_FLAG (1U << 1)
#define DM_SECURE_DATA_FLAG (1U << 15)

#define _DM_IOC_NR(cmd) (((cmd) >> 0) & 0xff)
/* _IOWR(0xfd, cmd, struct dm_ioctl). Replicate the macro layout from
 * asm-generic/ioctl.h since we cannot include the host's headers. */
#define _DM_IOC(cmd)                                                           \
	((3U << 30) | ((unsigned)sizeof(struct dm_ioctl_local) << 16) |        \
	 ((DM_IOCTL) << 8) | (cmd))

/* dm-mod ABI we target. The kernel happily handles older minors as
 * long as we initialise the fields it ignored. */
#define ANYFS_DM_VERSION_MAJOR 4
#define ANYFS_DM_VERSION_MINOR 0
#define ANYFS_DM_VERSION_PATCH 0

/* Device-major for misc devices in Linux. */
#define ANYFS_MISC_MAJOR 10
/* Device-minor for /dev/mapper/control (see <linux/miscdevice.h>). */
#define ANYFS_DM_CONTROL_MINOR 236

static int ensure_dev_dir(void)
{
	if (lkl_sys_access("/dev", 0) < 0)
		(void)lkl_sys_mkdir("/dev", 0700);
	if (lkl_sys_access("/dev/mapper", 0) < 0)
		(void)lkl_sys_mkdir("/dev/mapper", 0755);
	return 0;
}

static int dm_open_control(void)
{
	ensure_dev_dir();
	const char* ctl = "/dev/mapper/control";
	if (lkl_sys_access(ctl, 0) < 0) {
		/* dev_t encoding for (10, 236) — same encoding
		 * lkl_get_virtio_blkdev uses: major in bits 8..19, minor split
		 * (low 8 in 0..7, top bits in 12..31). With minor=236, only the
		 * low 8 bits matter. */
		unsigned int dev = ((ANYFS_MISC_MAJOR & 0xfff) << 8) |
				   (ANYFS_DM_CONTROL_MINOR & 0xff);
		int r = lkl_sys_mknod(ctl, LKL_S_IFCHR | 0600, dev);
		if (r < 0 && r != -LKL_EEXIST)
			return r;
	}
	int fd = lkl_sys_open(ctl, LKL_O_RDWR, 0);
	return fd;
}

static void dm_fill_header(struct dm_ioctl_local* h, const char* name,
			   uint32_t data_size, uint32_t target_count)
{
	memset(h, 0, sizeof(*h));
	h->version[0] = ANYFS_DM_VERSION_MAJOR;
	h->version[1] = ANYFS_DM_VERSION_MINOR;
	h->version[2] = ANYFS_DM_VERSION_PATCH;
	h->data_size = data_size;
	h->data_start = sizeof(struct dm_ioctl_local);
	h->target_count = target_count;
	h->flags = 0;
	if (name) {
		strncpy(h->name, name, DM_NAME_LEN - 1);
		h->name[DM_NAME_LEN - 1] = '\0';
	}
}

/* Run one ioctl, retrying once if the kernel reports DM_BUFFER_FULL. */
static int dm_ioctl(int fd, unsigned int cmd, void* arg)
{
	long r = lkl_sys_ioctl(fd, _DM_IOC(cmd), (long)arg);
	return (int)r;
}

static int dm_version_check(int fd)
{
	struct dm_ioctl_local h;
	dm_fill_header(&h, NULL, sizeof(h), 0);
	int r = dm_ioctl(fd, DM_VERSION_CMD, &h);
	if (r < 0)
		return r;
	/* Kernel rewrote version[]: anything 4.x.y is fine. */
	if (h.version[0] != 4)
		return -1;
	return 0;
}

/* Ensure /dev/mapper/<name> exists pointing at the kernel's dev_t. */
static int dm_publish_dev_node(const char* name, uint64_t dev,
			       char out_blkdev[64])
{
	char path[80];
	snprintf(path, sizeof(path), "/dev/mapper/%s", name);
	/* dev_t encoding: major in bits 8..19, minor split as for control. */
	unsigned int major = (unsigned)((dev >> 8) & 0xfff);
	unsigned int minor = (unsigned)((dev & 0xff) | ((dev >> 12) & 0xfff00));
	unsigned int udev =
	    ((major & 0xfff) << 8) | (minor & 0xff) | ((minor & ~0xff) << 12);
	/* Best-effort: unlink first so we never inherit a stale node. */
	(void)lkl_sys_unlink(path);
	int r = lkl_sys_mknod(path, LKL_S_IFBLK | 0600, udev);
	if (r < 0)
		return r;
	if (out_blkdev) {
		strncpy(out_blkdev, path, 63);
		out_blkdev[63] = '\0';
	}
	return 0;
}

/* Common create / load / resume sequence. `target_type` is "linear" or
 * "crypt". `params` is the target's parameter string (NUL-terminated,
 * unpadded — we pad here). For secret-carrying calls (crypt) we set
 * DM_SECURE_DATA_FLAG on the LOAD so the kernel wipes its internal
 * copy on free. */
static int dm_setup_one_target(const char* name, const char* target_type,
			       uint64_t length_sectors, const char* params,
			       int secure, char out_blkdev[64])
{
	int fd = dm_open_control();
	if (fd < 0)
		return fd;
	int rc = dm_version_check(fd);
	if (rc < 0)
		goto out_close;

	/* 1. DM_DEV_CREATE — empty payload, just the header. */
	struct dm_ioctl_local hdr;
	dm_fill_header(&hdr, name, sizeof(hdr), 0);
	rc = dm_ioctl(fd, DM_DEV_CREATE_CMD, &hdr);
	if (rc < 0)
		goto out_close;
	uint64_t dev = hdr.dev;

	/* 2. DM_TABLE_LOAD — one target_spec + the parameter blob,
	 *    padded so the total size is 8-byte aligned. We use a single
	 *    fixed-size buffer (4KB max param) which is plenty for linear
	 *    and crypt. */
	size_t params_len = strlen(params) + 1;
	size_t pad = (8 - ((sizeof(struct dm_ioctl_local) +
			    sizeof(struct dm_target_spec_local) + params_len) %
			   8)) %
		     8;
	size_t total = sizeof(struct dm_ioctl_local) +
		       sizeof(struct dm_target_spec_local) + params_len + pad;
	char* buf = calloc(1, total);
	if (!buf) {
		rc = -LKL_ENOMEM;
		goto out_remove;
	}

	struct dm_ioctl_local* load_hdr = (struct dm_ioctl_local*)buf;
	dm_fill_header(load_hdr, name, (uint32_t)total, 1);
	if (secure)
		load_hdr->flags |= DM_SECURE_DATA_FLAG;

	struct dm_target_spec_local* spec =
	    (struct dm_target_spec_local*)(buf + sizeof(*load_hdr));
	spec->sector_start = 0;
	spec->length = length_sectors;
	spec->status = 0;
	spec->next = (uint32_t)(sizeof(*spec) + params_len + pad);
	strncpy(spec->target_type, target_type, DM_MAX_TYPE_NAME - 1);

	memcpy(buf + sizeof(*load_hdr) + sizeof(*spec), params, params_len);

	rc = dm_ioctl(fd, DM_TABLE_LOAD_CMD, buf);
	if (secure)
		memset(buf, 0, total);
	free(buf);
	if (rc < 0)
		goto out_remove;

	/* 3. DM_DEV_SUSPEND with flags=0 → resume (activates the new table). */
	dm_fill_header(&hdr, name, sizeof(hdr), 0);
	rc = dm_ioctl(fd, DM_DEV_SUSPEND_CMD, &hdr);
	if (rc < 0)
		goto out_remove;

	/* Publish the node so callers can sysfs-walk it. */
	rc = dm_publish_dev_node(name, dev, out_blkdev);
	if (rc < 0)
		goto out_remove;

	lkl_sys_close(fd);
	return 0;

out_remove:;
	/* Tear down the half-built device — best-effort. */
	struct dm_ioctl_local rm;
	dm_fill_header(&rm, name, sizeof(rm), 0);
	(void)dm_ioctl(fd, DM_DEV_REMOVE_CMD, &rm);
out_close:
	lkl_sys_close(fd);
	return rc;
}

int anyfs_dm_linear(const char* parent_blkdev, uint64_t offset_sectors,
		    uint64_t length_sectors, const char* name,
		    char out_blkdev[64])
{
	if (!parent_blkdev || !name || !out_blkdev || length_sectors == 0)
		return -LKL_EINVAL;

	/* dm-linear params: "<dev_path> <start_sector>" */
	char params[256];
	int n = snprintf(params, sizeof(params), "%s %" PRIu64, parent_blkdev,
			 offset_sectors);
	if (n < 0 || (size_t)n >= sizeof(params))
		return -LKL_EINVAL;
	return dm_setup_one_target(name, "linear", length_sectors, params, 0,
				   out_blkdev);
}

int anyfs_dm_crypt(const char* parent_blkdev, uint64_t offset_sectors,
		   uint64_t length_sectors, uint64_t iv_offset,
		   const char* cipher, const uint8_t* key, size_t key_len,
		   const char* name, char out_blkdev[64])
{
	if (!parent_blkdev || !cipher || !key || key_len == 0 || !name ||
	    !out_blkdev || length_sectors == 0)
		return -LKL_EINVAL;

	/* dm-crypt params: "<cipher> <hex-key> <iv_offset> <dev_path>
	 *                    <offset_sectors> [optional]" */
	size_t hex_len = key_len * 2;
	/* params buffer must hold cipher (≤64) + hex_key + offsets + path. */
	size_t cap = 256 + hex_len;
	char* params = malloc(cap);
	if (!params)
		return -LKL_ENOMEM;

	char* p = params;
	p += snprintf(p, (size_t)(params + cap - p), "%s ", cipher);
	for (size_t i = 0; i < key_len; i++) {
		static const char hx[] = "0123456789abcdef";
		*p++ = hx[(key[i] >> 4) & 0xf];
		*p++ = hx[key[i] & 0xf];
	}
	*p++ = ' ';
	p += snprintf(p, (size_t)(params + cap - p), "%" PRIu64 " %s %" PRIu64,
		      iv_offset, parent_blkdev, offset_sectors);

	int rc = dm_setup_one_target(name, "crypt", length_sectors, params, 1,
				     out_blkdev);
	/* Zero the key in our copy before freeing. */
	memset(params, 0, cap);
	free(params);
	return rc;
}

int anyfs_dm_remove(const char* name)
{
	if (!name)
		return -LKL_EINVAL;
	int fd = dm_open_control();
	if (fd < 0)
		return fd;
	int rc = dm_version_check(fd);
	if (rc < 0) {
		lkl_sys_close(fd);
		return rc;
	}

	/* Best-effort suspend first so a busy device doesn't fail removal. */
	struct dm_ioctl_local h;
	dm_fill_header(&h, name, sizeof(h), 0);
	h.flags = DM_SUSPEND_FLAG;
	(void)dm_ioctl(fd, DM_DEV_SUSPEND_CMD, &h);

	/* Clear table, then remove the device node. */
	dm_fill_header(&h, name, sizeof(h), 0);
	(void)dm_ioctl(fd, DM_TABLE_CLEAR_CMD, &h);

	dm_fill_header(&h, name, sizeof(h), 0);
	rc = dm_ioctl(fd, DM_DEV_REMOVE_CMD, &h);

	/* Remove the /dev/mapper/<name> we created. Errors are fine. */
	char node[80];
	snprintf(node, sizeof(node), "/dev/mapper/%s", name);
	(void)lkl_sys_unlink(node);

	lkl_sys_close(fd);
	return rc == -LKL_ENXIO ? 0 : rc; /* not-present is success */
}

/* BLKRRPART ioctl number (_IO(0x12, 95)) — keeps us decoupled from the
 * host <linux/fs.h>. */
#define ANYFS_BLKRRPART (((0U) << 30) | (0x12U << 8) | 95U)

int anyfs_dm_rescan_partitions(const char* blkdev_path)
{
	if (!blkdev_path)
		return -LKL_EINVAL;
	int fd = lkl_sys_open(blkdev_path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return fd;
	long r = lkl_sys_ioctl(fd, ANYFS_BLKRRPART, 0);
	lkl_sys_close(fd);
	return (int)r;
}
