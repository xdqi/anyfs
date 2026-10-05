/*
 * anyfs_sysfs.c — Walk LKL's /sys/block/<vdN>/ for partition geometry.
 *
 * The kernel has already parsed the partition table (MBR/GPT/extended)
 * by the time we get here, so all partitions — including MBR logical
 * partitions starting at p5 — appear as subdirs.
 */
#define _GNU_SOURCE
#include "anyfs_sysfs.h"

#include <lkl.h>
#include <lkl_host.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read up to `cap-1` bytes from an LKL-visible path and null-terminate. */
static int read_small(const char* path, char* buf, size_t cap)
{
	int fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	long n = lkl_sys_read(fd, buf, cap - 1);
	lkl_sys_close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	/* Trim trailing newline. */
	while (n > 0 &&
	       (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return (int)n;
}

static int read_u64(const char* path, uint64_t* out)
{
	char buf[64];
	if (read_small(path, buf, sizeof(buf)) < 0)
		return -1;
	char* end = NULL;
	unsigned long long v = strtoull(buf, &end, 10);
	if (end == buf)
		return -1;
	*out = (uint64_t)v;
	return 0;
}

int anyfs_sysfs_resolve_disk_name(uint32_t dev, char out[64])
{
	/* LKL's dev_t encoding matches Linux: high 12 bits of upper word
	 * are major, low bits of lower word are minor. lkl_get_virtio_blkdev
	 * returns the kernel's dev_t directly. */
	unsigned int major = (dev >> 8) & 0xfff;
	unsigned int minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	char link_path[64];
	snprintf(link_path, sizeof(link_path), "/sys/dev/block/%u:%u", major,
		 minor);

	char target[256];
	long n = lkl_sys_readlink(link_path, target, sizeof(target) - 1);
	if (n <= 0)
		return -1;
	target[n] = '\0';

	/* target like "../../devices/.../block/vda" — basename. */
	char* slash = strrchr(target, '/');
	const char* name = slash ? slash + 1 : target;
	if (!*name)
		return -1;

	strncpy(out, name, 64 - 1);
	out[64 - 1] = '\0';
	return 0;
}

/* Copy the first entry of `dir` whose name starts with `prefix` into out
 * and return 0; with out == NULL, return how many entries match instead.
 * Negative if `dir` cannot be opened. */
static int scan_prefix(const char* dir, const char* prefix, char* out,
		       size_t cap)
{
	int err = 0;
	struct lkl_dir* d = lkl_opendir(dir, &err);
	if (!d)
		return -1;
	size_t pl = strlen(prefix);
	int count = 0, ret = -1;
	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(d)) != NULL) {
		if (strncmp(de->d_name, prefix, pl) != 0)
			continue;
		if (!out) {
			count++;
			continue;
		}
		snprintf(out, cap, "%s", de->d_name);
		ret = 0;
		break;
	}
	lkl_closedir(d);
	return out ? ret : count;
}

int anyfs_sysfs_virtio_disk_name(int disk_id, char* out, size_t cap)
{
	char path[192], virtio[64];

	if (disk_id < 0 || !out || !cap)
		return -1;
	/* Devices registered before boot sit under virtio-mmio-cmdline and
	 * take the first disk ids; hot-added ones are platform devices
	 * numbered from 0 after them (see LKL's virtio_dev_setup). */
	int nboot = scan_prefix("/sys/devices/virtio-mmio-cmdline",
				"virtio-mmio.", NULL, 0);
	if (nboot < 0)
		nboot = 0;
	if (disk_id < nboot)
		snprintf(path, sizeof(path),
			 "/sys/devices/virtio-mmio-cmdline/virtio-mmio.%d",
			 disk_id);
	else
		snprintf(path, sizeof(path),
			 "/sys/devices/platform/virtio-mmio.%d.auto",
			 disk_id - nboot);
	if (scan_prefix(path, "virtio", virtio, sizeof(virtio)) < 0)
		return -1;
	size_t len = strlen(path);
	snprintf(path + len, sizeof(path) - len, "/%s/block", virtio);
	return scan_prefix(path, "vd", out, cap);
}

int anyfs_sysfs_walk(const char* disk_name, AnyfsSysfsPart* buf, size_t buf_n)
{
	if (!disk_name || !buf)
		return -1;

	char dir_path[128];
	snprintf(dir_path, sizeof(dir_path), "/sys/block/%s", disk_name);

	int err = 0;
	struct lkl_dir* dir = lkl_opendir(dir_path, &err);
	if (!dir)
		return -1;

	size_t got = 0;
	size_t dn_len = strlen(disk_name);
	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		const char* n = de->d_name;
		if (n[0] == '.')
			continue;
		/* Subdir must start with <disk_name> as a prefix. */
		if (strncmp(n, disk_name, dn_len) != 0)
			continue;
		const char* rest = n + dn_len;
		/* Conventional kernel names: "vda1", "vda12", or with the
		 * partition separator "nvme0n1p1". For virtio/sd/vd the rest
		 * is digits only. Tolerate an optional leading 'p'. */
		if (*rest == 'p')
			rest++;
		if (!*rest)
			continue;
		char* end = NULL;
		unsigned long idx = strtoul(rest, &end, 10);
		if (!end || *end != '\0' || idx == 0)
			continue;

		if (got >= buf_n) {
			/* Out of buffer; keep counting how many we'd have. */
			got++;
			continue;
		}

		AnyfsSysfsPart* p = &buf[got];
		memset(p, 0, sizeof(*p));
		p->index = (unsigned int)idx;
		strncpy(p->name, n, sizeof(p->name) - 1);

		char attr[192];
		uint64_t v = 0;
		snprintf(attr, sizeof(attr), "%s/%s/start", dir_path, n);
		if (read_u64(attr, &v) == 0)
			p->start_bytes = v * 512u;
		snprintf(attr, sizeof(attr), "%s/%s/size", dir_path, n);
		if (read_u64(attr, &v) == 0)
			p->size_bytes = v * 512u;
		snprintf(attr, sizeof(attr), "%s/%s/ro", dir_path, n);
		if (read_u64(attr, &v) == 0)
			p->read_only = (v != 0);

		got++;
	}
	lkl_closedir(dir);

	/* Sort by index — readdir order is not guaranteed. */
	if (got > 1 && got <= buf_n) {
		for (size_t i = 1; i < got; i++) {
			AnyfsSysfsPart tmp = buf[i];
			size_t j = i;
			while (j > 0 && buf[j - 1].index > tmp.index) {
				buf[j] = buf[j - 1];
				j--;
			}
			buf[j] = tmp;
		}
	}

	return (int)got;
}
