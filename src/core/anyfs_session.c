/*
 * anyfs_disk.c — Multi-partition session layer.
 *
 * State machine per partition: NEW -> MOUNTING -> MOUNTED | FAILED.
 * One mutex per disk for the table; one condvar per partition so
 * concurrent enter()s serialise without holding the table lock during
 * the actual anyfs_mount (NTFS/btrfs mounts can be slow).
 *
 * v2: the slot array is tree-shaped. Top-level partitions of the
 * outer disk have parent_slot = -1; container slots (NESTED / LVM_PV /
 * LUKS) materialise their children inside the same array on enter.
 * Children carry parent_slot = parent's index. Lookup is by
 * (parent_slot, index) tuple.
 */
#define _GNU_SOURCE
#include "anyfs_session.h"

#include "anyfs_backend.h"
#include "anyfs_container.h"
#include "anyfs_dm.h"
#include "anyfs_internal.h"
#include "anyfs_mount.h"
#include "anyfs_path.h"
#include "anyfs_probe.h"
#include "anyfs_sysfs.h"

#include <lkl.h>
#include <lkl_host.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Set ANYFS_DEBUG=1 in the environment to see slot lifecycle traces. */
int anyfs_dbg(void)
{
	static int v = -1;
	if (v < 0) {
		const char* e = getenv("ANYFS_DEBUG");
		v = (e && *e && *e != '0') ? 1 : 0;
	}
	return v;
}

const char* anyfs_partkind_name(AnyfsPartKind k)
{
	switch (k) {
	case ANYFS_PART_KIND_FS:
		return "FS";
	case ANYFS_PART_KIND_LVM_PV:
		return "LVM_PV";
	case ANYFS_PART_KIND_LUKS:
		return "LUKS";
	case ANYFS_PART_KIND_NESTED_PARTITION_TABLE:
		return "NESTED";
	case ANYFS_PART_KIND_UNKNOWN:
		return "UNKNOWN";
	}
	return "?";
}

void set_fail(PartSlot* p, const char* reason)
{
	free(p->fail_reason);
	p->fail_reason = reason ? strdup(reason) : NULL;
	p->state = ANYFS_PART_FAILED;
}

static const char* image_basename(const char* path)
{
	const char* slash = strrchr(path, '/');
#ifdef _WIN32
	const char* bs = strrchr(path, '\\');
	if (bs && (!slash || bs > slash))
		slash = bs;
#endif
	return slash ? slash + 1 : path;
}
/* Snapshot of an AnyfsPartInfo from a slot. Caller holds d->lock. */
static void slot_to_info(const PartSlot* p, AnyfsPartInfo* o)
{
	memset(o, 0, sizeof(*o));
	o->slot_id = p->slot_id;
	o->parent = p->parent_slot;
	o->index = p->index;
	o->offset_bytes = p->offset_bytes;
	o->size_bytes = p->size_bytes;
	memcpy(o->ptype, p->ptype, sizeof(o->ptype));
	o->kind = p->kind;
	strncpy(o->fstype, p->fstype_cache, sizeof(o->fstype) - 1);
	strncpy(o->label, p->label, sizeof(o->label) - 1);
	strncpy(o->uuid, p->uuid, sizeof(o->uuid) - 1);
}

int alloc_slot_locked(AnyfsSession* d, int parent_slot, unsigned int index,
		      uint64_t off, uint64_t size, const char* blkdev,
		      AnyfsPartKind kind)
{
	if (d->n_parts >= d->parts_cap)
		return -1;
	int id = (int)d->n_parts++;
	PartSlot* p = &d->parts[id];
	memset(p, 0, sizeof(*p));
	p->slot_id = id;
	p->parent_slot = parent_slot;
	p->index = index;
	p->offset_bytes = off;
	p->size_bytes = size;
	strcpy(p->ptype, "?");
	if (blkdev)
		strncpy(p->blkdev, blkdev, sizeof(p->blkdev) - 1);
	p->kind = kind;
	p->state = ANYFS_PART_NEW;
	pthread_cond_init(&p->cv, NULL);
	return id;
}

static int find_slot_by_pair_locked(AnyfsSession* d, int parent_slot,
				    unsigned int index)
{
	for (size_t i = 0; i < d->n_parts; i++) {
		if (d->parts[i].parent_slot == parent_slot &&
		    d->parts[i].index == index)
			return (int)i;
	}
	return -1;
}

/* Append top-level partitions from sysfs. Caller holds d->lock. */
/* Read device number from the already-mounted /sys instead of
 * lkl_get_virtio_blkdev which mounts a second sysfs instance and
 * hangs under ASYNCIFY+QEMU+fiber builds. */
static int get_blkdev_from_sys(int disk_id, unsigned int part, uint32_t* pdevid)
{
	char path[128];
	char vd[8];
	snprintf(vd, sizeof(vd), "vd%c", 'a' + disk_id);
	if (part == 0)
		snprintf(path, sizeof(path), "/sys/block/%s/dev", vd);
	else
		snprintf(path, sizeof(path), "/sys/block/%s/%s%d/dev", vd, vd,
			 part);

	int fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	char buf[16];
	long n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	lkl_sys_close(fd);
	if (n <= 0)
		return -1;

	buf[n] = '\0';
	unsigned int major, minor;
	if (sscanf(buf, "%u:%u", &major, &minor) != 2)
		return -1;

	*pdevid = (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
	return 0;
}

static int load_top_parts_locked(AnyfsSession* d)
{
	AnyfsSysfsPart sbuf[MAX_PARTS];
	int n = anyfs_sysfs_walk(d->sysfs_name, sbuf, MAX_PARTS);
	if (n < 0)
		return -1;
	if ((size_t)n > MAX_PARTS)
		n = MAX_PARTS;

	for (int i = 0; i < n; i++) {
		char blkdev[80];
		snprintf(blkdev, sizeof(blkdev), "/dev/%s", sbuf[i].name);
		int sid = alloc_slot_locked(
		    d, ROOT_PARENT, sbuf[i].index, sbuf[i].start_bytes,
		    sbuf[i].size_bytes, blkdev, ANYFS_PART_KIND_FS);
		if (sid < 0)
			break;

		/* Best-effort mknod so kindprobe can pread the partition. */
		uint32_t dev = 0;
		if (get_blkdev_from_sys(d->disk_id, sbuf[i].index, &dev) == 0) {
			if (lkl_sys_access("/dev", 0) < 0)
				(void)lkl_sys_mkdir("/dev", 0700);
			(void)lkl_sys_mknod(blkdev, LKL_S_IFBLK | 0600, dev);
		}
		d->parts[sid].kind = anyfs_probe_kind_blkdev(blkdev);
		if (d->parts[sid].kind == ANYFS_PART_KIND_FS) {
			(void)anyfs_probe_meta(
			    blkdev, d->parts[sid].fstype_cache,
			    d->parts[sid].label, d->parts[sid].uuid);
		}
	}
	return 0;
}

int anyfs_session_open(const char* image_path, uint32_t flags,
		       AnyfsSession** out)
{
	if (!image_path || !out)
		return -1;
	*out = NULL;

	int disk_id = anyfs_disk_add(image_path, flags);
	if (disk_id < 0)
		return -1;

	AnyfsSession* d = (AnyfsSession*)calloc(1, sizeof(*d));
	if (!d) {
		anyfs_disk_remove(disk_id);
		return -1;
	}
	d->disk_id = disk_id;
	d->open_flags = flags;
	strncpy(d->image_path, image_path, sizeof(d->image_path) - 1);
	strncpy(d->display, image_basename(image_path), sizeof(d->display) - 1);
	pthread_mutex_init(&d->lock, NULL);

	d->parts_cap = MAX_PARTS;
	d->parts = (PartSlot*)calloc(d->parts_cap, sizeof(PartSlot));
	if (!d->parts) {
		anyfs_disk_remove(disk_id);
		pthread_mutex_destroy(&d->lock);
		free(d);
		return -1;
	}
	d->n_parts = 0;

	/* Yield to the event loop to flush any stale ASYNCIFY fiber
	 * state left over from QEMU block I/O during lkl_disk_add.
	 * Without this checkpoint, subsequent sysfs reads may hang. */
#if defined(__EMSCRIPTEN__) && defined(ANYFS_HAS_QEMU)
	emscripten_sleep(0);
#endif
	/* Construct sysfs block name from disk_id. The first LKL
	 * virtio-blk device is always "vda", second "vdb", etc.
	 * Reading from the already-mounted /sys avoids the hang
	 * that lkl_get_virtio_blkdev triggers in ASYNCIFY builds. */
	snprintf(d->sysfs_name, sizeof(d->sysfs_name), "vd%c", 'a' + disk_id);

	pthread_mutex_lock(&d->lock);
	(void)load_top_parts_locked(d);
	pthread_mutex_unlock(&d->lock);

	/* Cache the whole-disk fstype via libblkid (anyfs_probe_meta), which
	 * recognises every filesystem blkid knows — btrfs, f2fs, bcachefs, …,
	 * not just the handful of superblock magics this used to hand-check.
	 * mountWhole reuses the cached hint and avoids any probe reads that
	 * would corrupt fibers. blkid runs against a host-spooled snapshot of
	 * the device, so it's ASYNCIFY-safe like the per-partition probe above.
	 * (If blkid can't identify the device the hint stays empty → whole-disk
	 * mount falls back to auto-detection.) */
	d->whole_fstype_hint[0] = '\0';
	d->whole_dev = 0;
	uint32_t probe_dev = 0;
	if (get_blkdev_from_sys(d->disk_id, 0, &probe_dev) == 0) {
		d->whole_dev = probe_dev;
		if (lkl_sys_access("/dev", 0) < 0)
			lkl_sys_mkdir("/dev", 0700);
		char tmpdev[80];
		snprintf(tmpdev, sizeof(tmpdev), "/dev/.anyfs_probe_%d",
			 d->disk_id);
		(void)lkl_sys_mknod(tmpdev, LKL_S_IFBLK | 0600, probe_dev);
		char label[64], uuid[40];
		(void)anyfs_probe_meta(tmpdev, d->whole_fstype_hint, label,
				       uuid);
		lkl_sys_unlink(tmpdev);
	}

	*out = d;
	return 0;
}

void anyfs_session_close(AnyfsSession* d)
{
	if (!d)
		return;

	/* Tear down in reverse order so children unmount before parents. */
	if (d->parts) {
		for (ssize_t i = (ssize_t)d->n_parts - 1; i >= 0; i--) {
			PartSlot* p = &d->parts[i];
			if (p->state == ANYFS_PART_MOUNTED &&
			    p->kind == ANYFS_PART_KIND_FS) {
				char name[64];
				if (p->parent_slot == ROOT_PARENT)
					snprintf(name, sizeof(name),
						 "anyfs_d%d_p%u", d->disk_id,
						 p->index);
				else
					snprintf(name, sizeof(name),
						 "anyfs_d%d_s%d", d->disk_id,
						 p->slot_id);
				anyfs_umount(name);
			}
			if (p->dm_name[0])
				(void)anyfs_dm_remove(p->dm_name);
			pthread_cond_destroy(&p->cv);
			free(p->fail_reason);
		}
		free(d->parts);
	}

	if (d->disk_id >= 0)
		anyfs_disk_remove(d->disk_id);
	pthread_mutex_destroy(&d->lock);
	free(d);
}

const char* anyfs_session_display(const AnyfsSession* d)
{
	return d ? d->display : "";
}
int anyfs_session_id(const AnyfsSession* d)
{
	return d ? d->disk_id : -1;
}

const char* anyfs_session_whole_fstype_hint(const AnyfsSession* d)
{
	if (!d || !d->whole_fstype_hint[0])
		return NULL;
	return d->whole_fstype_hint;
}

uint32_t anyfs_session_whole_dev(const AnyfsSession* d)
{
	return d ? d->whole_dev : 0;
}

int anyfs_session_meta(AnyfsSession* d, AnyfsSessionMeta* out)
{
	if (!d || !out)
		return -1;
	out->logical_size = 0;
	out->pt_type[0] = '\0';

	/* Logical size: /sys/block/<vda>/size is in 512-byte sectors. */
	char sysfs_path[128];
	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/block/%s/size",
		 d->sysfs_name);
	int fd = lkl_sys_open(sysfs_path, LKL_O_RDONLY, 0);
	if (fd >= 0) {
		char sbuf[64];
		long n = lkl_sys_read(fd, sbuf, sizeof(sbuf) - 1);
		lkl_sys_close(fd);
		if (n > 0) {
			sbuf[n] = '\0';
			char* end = NULL;
			unsigned long long sectors = strtoull(sbuf, &end, 10);
			if (end != sbuf)
				out->logical_size = (uint64_t)sectors * 512u;
		}
	}

	/* PT flavour: probe the start of /dev/<vda>. The partition setup path
	 * mknods /dev/<vdaN> per partition; the whole-disk node may not exist
	 * yet, so create it best-effort. */
	char blk_path[80];
	snprintf(blk_path, sizeof(blk_path), "/dev/%s", d->sysfs_name);
	uint32_t whole_dev = 0;
	if (get_blkdev_from_sys(d->disk_id, 0, &whole_dev) == 0) {
		if (lkl_sys_access("/dev", 0) < 0)
			(void)lkl_sys_mkdir("/dev", 0700);
		(void)lkl_sys_mknod(blk_path, LKL_S_IFBLK | 0600, whole_dev);
	}
	const char* pt = anyfs_probe_pttype_blkdev(blk_path);
	strncpy(out->pt_type, pt, sizeof(out->pt_type) - 1);
	out->pt_type[sizeof(out->pt_type) - 1] = '\0';
	return 0;
}

int anyfs_session_list(AnyfsSession* d, int parent_slot_id, AnyfsPartInfo* buf,
		       size_t buf_n, size_t* got)
{
	if (!d)
		return -1;
	size_t total = 0;
	size_t written = 0;
	pthread_mutex_lock(&d->lock);
	for (size_t i = 0; i < d->n_parts; i++) {
		if (d->parts[i].parent_slot != parent_slot_id)
			continue;
		if (buf && written < buf_n)
			slot_to_info(&d->parts[i], &buf[written++]);
		total++;
	}
	if (got)
		*got = total;
	pthread_mutex_unlock(&d->lock);
	return (int)written;
}

size_t anyfs_session_count(AnyfsSession* d, int parent_slot_id)
{
	if (!d)
		return 0;
	size_t n = 0;
	pthread_mutex_lock(&d->lock);
	for (size_t i = 0; i < d->n_parts; i++)
		if (d->parts[i].parent_slot == parent_slot_id)
			n++;
	pthread_mutex_unlock(&d->lock);
	return n;
}

AnyfsPartState anyfs_session_state(AnyfsSession* d, unsigned int part)
{
	if (!d)
		return ANYFS_PART_FAILED;
	pthread_mutex_lock(&d->lock);
	int sid = find_slot_by_pair_locked(d, ROOT_PARENT, part);
	AnyfsPartState s = sid >= 0 ? d->parts[sid].state : ANYFS_PART_FAILED;
	pthread_mutex_unlock(&d->lock);
	return s;
}

AnyfsPartState anyfs_session_state_slot(AnyfsSession* d, int slot_id)
{
	if (!d || slot_id < 0)
		return ANYFS_PART_FAILED;
	pthread_mutex_lock(&d->lock);
	AnyfsPartState s = ((size_t)slot_id < d->n_parts)
			       ? d->parts[slot_id].state
			       : ANYFS_PART_FAILED;
	pthread_mutex_unlock(&d->lock);
	return s;
}

const char* anyfs_session_fail_reason(AnyfsSession* d, unsigned int part)
{
	if (!d)
		return NULL;
	pthread_mutex_lock(&d->lock);
	int sid = find_slot_by_pair_locked(d, ROOT_PARENT, part);
	const char* r = NULL;
	if (sid >= 0 && d->parts[sid].state == ANYFS_PART_FAILED)
		r = d->parts[sid].fail_reason;
	pthread_mutex_unlock(&d->lock);
	return r;
}

const char* anyfs_session_fail_reason_slot(AnyfsSession* d, int slot_id)
{
	if (!d || slot_id < 0)
		return NULL;
	pthread_mutex_lock(&d->lock);
	const char* r = NULL;
	if ((size_t)slot_id < d->n_parts &&
	    d->parts[slot_id].state == ANYFS_PART_FAILED)
		r = d->parts[slot_id].fail_reason;
	pthread_mutex_unlock(&d->lock);
	return r;
}

/* Mount a KIND_FS slot. Caller must NOT hold the lock. */
static int enter_fs_slot(AnyfsSession* d, int slot_id, uint32_t flags,
			 char lkl_path[ANYFS_LKL_PATH_MAX])
{
	pthread_mutex_lock(&d->lock);
	PartSlot* p = &d->parts[slot_id];

	while (p->state == ANYFS_PART_MOUNTING)
		pthread_cond_wait(&p->cv, &d->lock);

	if (p->state == ANYFS_PART_MOUNTED) {
		if (p->parent_slot == ROOT_PARENT)
			snprintf(lkl_path, 64, "/lklmnt/anyfs_d%d_p%u",
				 d->disk_id, p->index);
		else
			snprintf(lkl_path, 64, "/lklmnt/anyfs_d%d_s%d",
				 d->disk_id, p->slot_id);
		pthread_mutex_unlock(&d->lock);
		return 0;
	}
	if (p->state == ANYFS_PART_FAILED) {
		pthread_mutex_unlock(&d->lock);
		return -3;
	}

	p->state = ANYFS_PART_MOUNTING;

	char mount_name[64];
	char dev_path[80];
	int is_top = (p->parent_slot == ROOT_PARENT);
	unsigned int top_part_idx = p->index;
	if (is_top)
		snprintf(mount_name, sizeof(mount_name), "anyfs_d%d_p%u",
			 d->disk_id, p->index);
	else
		snprintf(mount_name, sizeof(mount_name), "anyfs_d%d_s%d",
			 d->disk_id, p->slot_id);
	strncpy(dev_path, p->blkdev, sizeof(dev_path) - 1);
	dev_path[sizeof(dev_path) - 1] = '\0';
	const char* fstype_hint = p->fstype_cache[0] ? p->fstype_cache : "auto";
	pthread_mutex_unlock(&d->lock);

	AnyfsMount mnt = {0};
	uint32_t mflags = 0;
	if ((d->open_flags & ANYFS_SESSION_READONLY) ||
	    (flags & ANYFS_MOUNT_RDONLY))
		mflags |= ANYFS_MOUNT_RDONLY;

	int rc;
	if (is_top) {
		rc = anyfs_mount(d->disk_id, top_part_idx, fstype_hint,
				 mount_name, mflags, &mnt);
	} else {
		/* Child of a container — go through anyfs_mount_blkdev so the
		 * dm-backed device path is used directly. */
		rc = anyfs_mount_blkdev(dev_path, fstype_hint, mount_name,
					mflags, &mnt);
	}

	pthread_mutex_lock(&d->lock);
	if (rc == 0) {
		p->state = ANYFS_PART_MOUNTED;
		strncpy(p->fstype_cache, mnt.fstype,
			sizeof(p->fstype_cache) - 1);
		snprintf(lkl_path, 64, "%s", mnt.mount_point);
	} else {
		char rbuf[64];
		snprintf(rbuf, sizeof(rbuf), "mount failed (rc=%d)", rc);
		set_fail(p, rbuf);
	}
	pthread_cond_broadcast(&p->cv);
	pthread_mutex_unlock(&d->lock);
	return rc;
}

int anyfs_session_enter(AnyfsSession* d, unsigned int part, uint32_t flags,
			char lkl_path[ANYFS_LKL_PATH_MAX])
{
	if (!d || !lkl_path)
		return -1;
	lkl_path[0] = '\0';

	/* p0 = whole disk (no partition table).
	 * Mount the entire block device as a single filesystem using cached
	 * dev_t + fstype hint from anyfs_session_open. */
	if (part == 0) {
		uint32_t dev = d->whole_dev;
		if (dev == 0)
			return -4;

		lkl_sys_access("/dev", 0) < 0 && lkl_sys_mkdir("/dev", 0700);
		char devpath[80];
		snprintf(devpath, sizeof(devpath), "/dev/anyfs_d%d_whole",
			 d->disk_id);
		(void)lkl_sys_mknod(devpath, LKL_S_IFBLK | 0600, dev);

		char name[64];
		snprintf(name, sizeof(name), "anyfs_d%d_whole", d->disk_id);

		const char* hint =
		    d->whole_fstype_hint[0] ? d->whole_fstype_hint : NULL;

		/* Inherit read-only from how the session was opened, exactly as
		 * the partition path (enter_fs_slot) does. A disk opened
		 * read-only (e.g. browser WORKERFS/URLFS, which have no writeback
		 * path) must mount its filesystem read-only too: a read-write
		 * ext4/ext3 mount runs jbd2 journal recovery, whose block writes
		 * fail on the non-writable backend (-EIO), aborting the mount.
		 * The RDONLY flag adds noload/norecovery so the mount skips
		 * recovery and succeeds. */
		uint32_t mflags = flags;
		if (d->open_flags & ANYFS_SESSION_READONLY)
			mflags |= ANYFS_MOUNT_RDONLY;

		AnyfsMount mnt = {0};
		int rc = anyfs_mount_blkdev(devpath, hint, name, mflags, &mnt);
		if (rc < 0) {
			lkl_sys_unlink(devpath);
			return rc;
		}
		snprintf(lkl_path, ANYFS_LKL_PATH_MAX, "%s", mnt.mount_point);
		return 0;
	}

	pthread_mutex_lock(&d->lock);
	int sid = find_slot_by_pair_locked(d, ROOT_PARENT, part);
	if (sid < 0) {
		pthread_mutex_unlock(&d->lock);
		return -2;
	}
	AnyfsPartKind kind = d->parts[sid].kind;
	pthread_mutex_unlock(&d->lock);

	if (kind == ANYFS_PART_KIND_FS)
		return enter_fs_slot(d, sid, flags, lkl_path);
	/* Container: bring it up (so children are materialised), but no
	 * mount path to return. */
	int rc = enter_container_slot(d, sid, NULL, flags);
	return rc;
}

int anyfs_session_walk(AnyfsSession* d, const struct AnyfsPathComp* comp,
		       size_t n_comp, uint32_t flags, int* leaf_slot_id_out,
		       char lkl_path[ANYFS_LKL_PATH_MAX])
{
	if (!d || !comp || n_comp == 0 || !lkl_path)
		return -LKL_EINVAL;
	lkl_path[0] = '\0';
	if (leaf_slot_id_out)
		*leaf_slot_id_out = -1;

	int parent = ROOT_PARENT;
	for (size_t i = 0; i < n_comp; i++) {
		pthread_mutex_lock(&d->lock);
		int sid = find_slot_by_pair_locked(d, parent, comp[i].p);
		pthread_mutex_unlock(&d->lock);
		if (sid < 0)
			return -LKL_ENOENT;

		if (i == n_comp - 1) {
			if (leaf_slot_id_out)
				*leaf_slot_id_out = sid;
			pthread_mutex_lock(&d->lock);
			AnyfsPartKind k = d->parts[sid].kind;
			pthread_mutex_unlock(&d->lock);
			if (k == ANYFS_PART_KIND_FS)
				return enter_fs_slot(d, sid, flags, lkl_path);
			/* Container leaf: materialise children, no mount. */
			return enter_container_slot(d, sid, comp[i].query,
						    flags);
		}

		/* Non-leaf must be a container. Bring it up. */
		int rc = enter_container_slot(d, sid, comp[i].query, flags);
		if (rc < 0)
			return rc;
		parent = sid;
	}
	return -LKL_EINVAL;
}

int anyfs_session_enter_path(AnyfsSession* d, const struct AnyfsPathComp* comp,
			     size_t n_comp, uint32_t flags,
			     char lkl_path[ANYFS_LKL_PATH_MAX])
{
	if (!d || !comp || n_comp == 0 || !lkl_path)
		return -LKL_EINVAL;
	/* Pre-check the leaf kind so callers that explicitly want a
	 * mounted FS get the historical -LKL_EISDIR signal when the path
	 * lands on a container. */
	int leaf = -1;
	int rc = anyfs_session_walk(d, comp, n_comp, flags, &leaf, lkl_path);
	if (rc < 0)
		return rc;
	if (leaf < 0)
		return -LKL_EINVAL;
	pthread_mutex_lock(&d->lock);
	AnyfsPartKind k = d->parts[leaf].kind;
	pthread_mutex_unlock(&d->lock);
	if (k != ANYFS_PART_KIND_FS)
		return -LKL_EISDIR;
	return 0;
}

int anyfs_session_probe(AnyfsSession* d, unsigned int part, char fstype[32],
			char label[64], uint64_t* used)
{
	if (!d)
		return -1;
	if (fstype)
		strcpy(fstype, "?");
	if (label)
		strcpy(label, "?");
	if (used)
		*used = 0;

	pthread_mutex_lock(&d->lock);
	int sid = find_slot_by_pair_locked(d, ROOT_PARENT, part);
	if (sid >= 0) {
		PartSlot* p = &d->parts[sid];
		if (p->fstype_cache[0] && fstype) {
			strncpy(fstype, p->fstype_cache, 31);
			fstype[31] = '\0';
		}
		if (p->label[0] && label) {
			strncpy(label, p->label, 63);
			label[63] = '\0';
		}
	}
	pthread_mutex_unlock(&d->lock);
	return 0;
}

int anyfs_session_leave(AnyfsSession* d, unsigned int part)
{
	if (!d)
		return -1;

	/* p0 = whole disk — no slot, unmount + unlink the synthetic /dev node.
	 */
	if (part == 0) {
		char name[64];
		snprintf(name, sizeof(name), "anyfs_d%d_whole", d->disk_id);
		int rc = anyfs_umount(name);
		char devpath[80];
		snprintf(devpath, sizeof(devpath), "/dev/anyfs_d%d_whole",
			 d->disk_id);
		(void)lkl_sys_unlink(devpath);
		return rc;
	}

	pthread_mutex_lock(&d->lock);
	int sid = find_slot_by_pair_locked(d, ROOT_PARENT, part);
	if (sid < 0 || d->parts[sid].state != ANYFS_PART_MOUNTED ||
	    d->parts[sid].kind != ANYFS_PART_KIND_FS) {
		pthread_mutex_unlock(&d->lock);
		return -1;
	}
	char name[64];
	snprintf(name, sizeof(name), "anyfs_d%d_p%u", d->disk_id, part);
	PartSlot* p = &d->parts[sid];
	pthread_mutex_unlock(&d->lock);

	int rc = anyfs_umount(name);

	pthread_mutex_lock(&d->lock);
	if (rc == 0)
		p->state = ANYFS_PART_NEW;
	pthread_mutex_unlock(&d->lock);
	return rc;
}
