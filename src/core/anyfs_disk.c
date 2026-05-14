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
#include "anyfs_disk.h"

#include "anyfs_dm.h"
#include "anyfs_sysfs.h"
#include "kindprobe.h"
#include "path_dsl.h"

#include <lkl.h>
#include <lkl_host.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Set ANYFS_DEBUG=1 in the environment to see slot lifecycle traces. */
static int anyfs_dbg(void)
{
	static int v = -1;
	if (v < 0) {
		const char* e = getenv("ANYFS_DEBUG");
		v = (e && *e && *e != '0') ? 1 : 0;
	}
	return v;
}
#define DBG(...)                                                               \
	do {                                                                   \
		if (anyfs_dbg())                                               \
			fprintf(stderr, "[anyfs] " __VA_ARGS__);               \
	} while (0)

#define MAX_PARTS 128
#define ROOT_PARENT (-1)

typedef struct {
	int slot_id;	    /* equals its own index in parts[] */
	int parent_slot;    /* -1 = top-level under the disk */
	unsigned int index; /* partition number under parent (1-based) */
	uint64_t offset_bytes;
	uint64_t size_bytes;
	char ptype[40];
	char blkdev[80];     /* "/dev/vda1" or "/dev/mapper/<name>" */
	char dm_name[64];    /* set only for DM-backed slots */
	char sysfs_name[64]; /* set only for slots that own children */
	AnyfsPartKind kind;
	AnyfsPartState state;
	char* fail_reason;
	int children_loaded; /* 1 once sysfs-walked after dm setup */
	char fstype_cache[32];
	char label[64];
	char uuid[40];
	pthread_cond_t cv;
} PartSlot;

struct AnyfsDisk {
	int disk_id;
	uint32_t open_flags;
	char image_path[512];
	char display[256];
	char sysfs_name[64]; /* "vda" */
	pthread_mutex_t lock;
	PartSlot* parts; /* size = parts_cap */
	size_t n_parts;
	size_t parts_cap;
};

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

static void set_fail(PartSlot* p, const char* reason)
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

/* Parse a `&`-separated `key=value` query string and resolve a LUKS
 * credential into `out` (size at most `out_cap`). The query is consumed
 * left-to-right and the first credential pair (`keyref` / `keyfile` /
 * `keyfd` / `key`) wins. Returns the number of key bytes resolved on
 * success, -1 on failure (with `errstr` populated), or 0 if no key
 * pair was present.
 *
 * v2 does not yet derive the LUKS master key from a passphrase; the
 * caller is expected to forward the resolved bytes to libcryptsetup
 * (v3) or to treat them as the volume key for direct dm-crypt setup.
 */
static int resolve_luks_credential(const char* query, unsigned char* out,
				   size_t out_cap, char errstr[160])
{
	errstr[0] = '\0';
	if (!query || !*query)
		return 0;

	char* dup = strdup(query);
	if (!dup) {
		snprintf(errstr, 160, "out of memory");
		return -1;
	}

	int got = 0;
	for (char* p = dup; *p;) {
		char* amp = strchr(p, '&');
		if (amp)
			*amp = '\0';
		char* eq = strchr(p, '=');
		if (eq) {
			*eq = '\0';
			const char *k = p, *v = eq + 1;
			if (strcmp(k, "key") == 0) {
				size_t n = strlen(v);
				if (n > out_cap)
					n = out_cap;
				memcpy(out, v, n);
				got = (int)n;
				fprintf(
				    stderr,
				    "anyfs: WARN literal `key=...' in argv is "
				    "visible to other processes; prefer "
				    "keyref=\n");
				break;
			} else if (strcmp(k, "keyref") == 0) {
				const char* e = getenv(v);
				if (!e) {
					snprintf(errstr, 160,
						 "keyref=%s: env var not set",
						 v);
					free(dup);
					return -1;
				}
				size_t n = strlen(e);
				if (n > out_cap)
					n = out_cap;
				memcpy(out, e, n);
				got = (int)n;
				break;
			} else if (strcmp(k, "keyfile") == 0) {
				int fd = open(v, O_RDONLY | O_CLOEXEC);
				if (fd < 0) {
					snprintf(errstr, 160,
						 "keyfile=%s: open failed "
						 "(errno=%d)",
						 v, errno);
					free(dup);
					return -1;
				}
				ssize_t n = read(fd, out, out_cap);
				close(fd);
				if (n <= 0) {
					snprintf(errstr, 160,
						 "keyfile=%s: read failed", v);
					free(dup);
					return -1;
				}
				got = (int)n;
				break;
			} else if (strcmp(k, "keyfd") == 0) {
				char* endp = NULL;
				long fd = strtol(v, &endp, 10);
				if (!endp || *endp != '\0' || fd < 0) {
					snprintf(errstr, 160,
						 "keyfd=%s: not a valid fd", v);
					free(dup);
					return -1;
				}
				ssize_t n = read((int)fd, out, out_cap);
				if (n <= 0) {
					snprintf(errstr, 160,
						 "keyfd=%ld: read failed", fd);
					free(dup);
					return -1;
				}
				got = (int)n;
				break;
			}
			if (eq)
				*eq = '=';
		}
		if (!amp)
			break;
		*amp = '&';
		p = amp + 1;
	}
	free(dup);
	return got;
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

static int alloc_slot_locked(AnyfsDisk* d, int parent_slot, unsigned int index,
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

static int find_slot_by_pair_locked(AnyfsDisk* d, int parent_slot,
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
static int load_top_parts_locked(AnyfsDisk* d)
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
		if (lkl_get_virtio_blkdev(d->disk_id, sbuf[i].index, &dev) ==
		    0) {
			if (lkl_sys_access("/dev", 0) < 0)
				(void)lkl_sys_mkdir("/dev", 0700);
			(void)lkl_sys_mknod(blkdev, LKL_S_IFBLK | 0600, dev);
		}
		d->parts[sid].kind = anyfs_kindprobe_blkdev(blkdev);
		if (d->parts[sid].kind == ANYFS_PART_KIND_FS) {
			(void)anyfs_kindprobe_meta(
			    blkdev, d->parts[sid].fstype_cache,
			    d->parts[sid].label, d->parts[sid].uuid);
		}
	}
	return 0;
}

int anyfs_disk_open(const char* image_path, uint32_t flags, AnyfsDisk** out)
{
	if (!image_path || !out)
		return -1;
	*out = NULL;

	int disk_id = anyfs_disk_add(image_path, flags);
	if (disk_id < 0)
		return -1;

	AnyfsDisk* d = (AnyfsDisk*)calloc(1, sizeof(*d));
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

	/* Resolve sysfs name via /sys/dev/block/<maj>:<min>. */
	uint32_t whole_dev = 0;
	int gv = lkl_get_virtio_blkdev(disk_id, 0, &whole_dev);
	if (gv < 0 ||
	    anyfs_sysfs_resolve_disk_name(whole_dev, d->sysfs_name) < 0) {
		d->sysfs_name[0] = '\0';
		*out = d;
		return 0;
	}

	pthread_mutex_lock(&d->lock);
	(void)load_top_parts_locked(d);
	pthread_mutex_unlock(&d->lock);

	*out = d;
	return 0;
}

void anyfs_disk_close(AnyfsDisk* d)
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

const char* anyfs_disk_display(const AnyfsDisk* d)
{
	return d ? d->display : "";
}
int anyfs_disk_id(const AnyfsDisk* d)
{
	return d ? d->disk_id : -1;
}

int anyfs_disk_list(AnyfsDisk* d, AnyfsPartInfo* buf, size_t buf_n, size_t* got)
{
	return anyfs_disk_list_children(d, ROOT_PARENT, buf, buf_n, got);
}

int anyfs_disk_list_children(AnyfsDisk* d, int parent_slot_id,
			     AnyfsPartInfo* buf, size_t buf_n, size_t* got)
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

size_t anyfs_disk_nparts(AnyfsDisk* d)
{
	if (!d)
		return 0;
	size_t n = 0;
	pthread_mutex_lock(&d->lock);
	for (size_t i = 0; i < d->n_parts; i++)
		if (d->parts[i].parent_slot == ROOT_PARENT)
			n++;
	pthread_mutex_unlock(&d->lock);
	return n;
}

AnyfsPartState anyfs_disk_state(AnyfsDisk* d, unsigned int part)
{
	if (!d)
		return ANYFS_PART_FAILED;
	pthread_mutex_lock(&d->lock);
	int sid = find_slot_by_pair_locked(d, ROOT_PARENT, part);
	AnyfsPartState s = sid >= 0 ? d->parts[sid].state : ANYFS_PART_FAILED;
	pthread_mutex_unlock(&d->lock);
	return s;
}

AnyfsPartState anyfs_disk_state_slot(AnyfsDisk* d, int slot_id)
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

const char* anyfs_disk_fail_reason(AnyfsDisk* d, unsigned int part)
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

const char* anyfs_disk_fail_reason_slot(AnyfsDisk* d, int slot_id)
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

/* Enter a container slot: parse the inner partition table directly,
 * create one dm-linear per inner partition, append child slots. The
 * kernel won't auto-scan partitions on a dm device (GENHD_FL_NO_PART),
 * so we own the parse. Caller must NOT hold the lock. */
static int enter_container_slot(AnyfsDisk* d, int slot_id, const char* query,
				uint32_t flags)
{
	(void)flags;
	pthread_mutex_lock(&d->lock);
	PartSlot* p = &d->parts[slot_id];

	while (p->state == ANYFS_PART_MOUNTING)
		pthread_cond_wait(&p->cv, &d->lock);
	if (p->state == ANYFS_PART_MOUNTED) {
		pthread_mutex_unlock(&d->lock);
		return 0;
	}
	if (p->state == ANYFS_PART_FAILED) {
		pthread_mutex_unlock(&d->lock);
		return -3;
	}
	p->state = ANYFS_PART_MOUNTING;

	/* Snapshot what we need so we can drop the lock. */
	AnyfsPartKind kind = p->kind;
	char parent_blkdev[80];
	strncpy(parent_blkdev, p->blkdev, sizeof(parent_blkdev) - 1);
	parent_blkdev[sizeof(parent_blkdev) - 1] = '\0';
	uint64_t length_sectors = p->size_bytes / 512;
	char dm_name[64];
	snprintf(dm_name, sizeof(dm_name), "anyfs_d%d_s%d_dm", d->disk_id,
		 p->slot_id);
	pthread_mutex_unlock(&d->lock);

	int rc;
	char out_blk[64];
	out_blk[0] = '\0';

	/* For NESTED we don't create a top-level dm device. Instead we parse
	 * the inner partition table ourselves and create one dm-linear per
	 * inner partition. The kernel won't auto-scan a dm device's PT
	 * (GENHD_FL_NO_PART is set on dm), so doing it ourselves is the
	 * supported recipe and works at any nesting depth. */
	AnyfsInnerPart inners[MAX_PARTS];
	int n_inner = 0;
	if (kind == ANYFS_PART_KIND_NESTED_PARTITION_TABLE) {
		n_inner =
		    anyfs_partprobe_blkdev(parent_blkdev, inners, MAX_PARTS);
		DBG("partprobe(%s) -> n=%d\n", parent_blkdev, n_inner);
		rc = (n_inner > 0) ? 0 : -LKL_ENOENT;
	} else if (kind == ANYFS_PART_KIND_LUKS) {
		(void)dm_name;
		(void)length_sectors;
		rc = -LKL_ENOSYS;
	} else if (kind == ANYFS_PART_KIND_LVM_PV) {
		rc = -LKL_ENOSYS;
	} else {
		rc = -LKL_EINVAL;
	}

	pthread_mutex_lock(&d->lock);
	if (rc < 0) {
		char buf[200];
		if (kind == ANYFS_PART_KIND_LUKS) {
			/* Parse the query so we can give the user actionable
			 * feedback even though we can't actually unlock yet. */
			unsigned char keybuf[128];
			char qerr[160];
			int kn = resolve_luks_credential(query, keybuf,
							 sizeof(keybuf), qerr);
			/* wipe key from memory immediately — we never use it */
			memset(keybuf, 0, sizeof(keybuf));
			if (kn < 0)
				snprintf(buf, sizeof(buf),
					 "LUKS credential: %s", qerr);
			else if (kn == 0)
				snprintf(
				    buf, sizeof(buf),
				    "LUKS: no credential in query; expected "
				    "keyref=ENV / keyfile=PATH / keyfd=N / "
				    "key=...");
			else
				snprintf(
				    buf, sizeof(buf),
				    "LUKS: credential resolved (%d bytes); "
				    "passphrase->masterkey derivation requires "
				    "v3 (libcryptsetup not yet linked)",
				    kn);
		} else if (kind == ANYFS_PART_KIND_LVM_PV) {
			snprintf(buf, sizeof(buf),
				 "LVM2 PV activation requires v3 (LVM metadata "
				 "parser)");
		} else {
			snprintf(buf, sizeof(buf),
				 "dm setup failed (kind=%s, rc=%d)",
				 anyfs_partkind_name(kind), rc);
		}
		set_fail(p, buf);
		pthread_cond_broadcast(&p->cv);
		pthread_mutex_unlock(&d->lock);
		return rc;
	}

	/* For NESTED: emit one dm-linear per inner partition, then append
	 * a child slot per inner. We hold d->lock here; the dm ioctls run
	 * outside, so drop briefly. */
	if (kind == ANYFS_PART_KIND_NESTED_PARTITION_TABLE) {
		pthread_mutex_unlock(&d->lock);
		for (int i = 0; i < n_inner; i++) {
			char cname[64];
			snprintf(cname, sizeof(cname), "anyfs_d%d_s%d_p%u",
				 d->disk_id, slot_id, inners[i].index);
			char cout[64];
			int crc = anyfs_dm_linear(
			    parent_blkdev, inners[i].start_bytes / 512,
			    inners[i].size_bytes / 512, cname, cout);
			DBG("dm_linear[child %u] start=%" PRIu64 " sz=%" PRIu64
			    " -> rc=%d node=%s\n",
			    inners[i].index, inners[i].start_bytes,
			    inners[i].size_bytes, crc, cout);
			if (crc < 0)
				continue;
			pthread_mutex_lock(&d->lock);
			int cid = alloc_slot_locked(
			    d, slot_id, inners[i].index, inners[i].start_bytes,
			    inners[i].size_bytes, cout, ANYFS_PART_KIND_FS);
			if (cid < 0) {
				pthread_mutex_unlock(&d->lock);
				/* Best-effort: tear down the orphaned dm
				 * device. */
				(void)anyfs_dm_remove(cname);
				continue;
			}
			strncpy(d->parts[cid].dm_name, cname,
				sizeof(d->parts[cid].dm_name) - 1);
			pthread_mutex_unlock(&d->lock);
			/* Probe outside the lock — pread + libblkid on a host
			 * tmpfile. */
			AnyfsPartKind ck = anyfs_kindprobe_blkdev(cout);
			char fst[32] = "", lbl[64] = "", uid[40] = "";
			if (ck == ANYFS_PART_KIND_FS)
				(void)anyfs_kindprobe_meta(cout, fst, lbl, uid);
			pthread_mutex_lock(&d->lock);
			d->parts[cid].kind = ck;
			strncpy(d->parts[cid].fstype_cache, fst,
				sizeof(d->parts[cid].fstype_cache) - 1);
			strncpy(d->parts[cid].label, lbl,
				sizeof(d->parts[cid].label) - 1);
			strncpy(d->parts[cid].uuid, uid,
				sizeof(d->parts[cid].uuid) - 1);
			pthread_mutex_unlock(&d->lock);
		}
		pthread_mutex_lock(&d->lock);
	} else {
		/* LUKS / LVM_PV path (currently always rc<0 above, so we never
		 * reach here) — kept for symmetry once v3 plumbing lands. */
		strncpy(p->dm_name, dm_name, sizeof(p->dm_name) - 1);
		p->dm_name[sizeof(p->dm_name) - 1] = '\0';
		strncpy(p->blkdev, out_blk, sizeof(p->blkdev) - 1);
		p->blkdev[sizeof(p->blkdev) - 1] = '\0';
	}
	p->children_loaded = 1;
	p->state = ANYFS_PART_MOUNTED;
	pthread_cond_broadcast(&p->cv);
	pthread_mutex_unlock(&d->lock);
	return 0;
}

/* Mount a KIND_FS slot. Caller must NOT hold the lock. */
static int enter_fs_slot(AnyfsDisk* d, int slot_id, uint32_t flags,
			 char lkl_path[64])
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
	if ((d->open_flags & ANYFS_DISK_READONLY) ||
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

int anyfs_disk_enter(AnyfsDisk* d, unsigned int part, uint32_t flags,
		     char lkl_path[64])
{
	if (!d || !lkl_path)
		return -1;
	lkl_path[0] = '\0';

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

int anyfs_disk_walk(AnyfsDisk* d, const struct AnyfsPathComp* comp,
		    size_t n_comp, uint32_t flags, int* leaf_slot_id_out,
		    char lkl_path[64])
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

int anyfs_disk_enter_path(AnyfsDisk* d, const struct AnyfsPathComp* comp,
			  size_t n_comp, uint32_t flags, char lkl_path[64])
{
	if (!d || !comp || n_comp == 0 || !lkl_path)
		return -LKL_EINVAL;
	/* Pre-check the leaf kind so callers that explicitly want a
	 * mounted FS get the historical -LKL_EISDIR signal when the path
	 * lands on a container. */
	int leaf = -1;
	int rc = anyfs_disk_walk(d, comp, n_comp, flags, &leaf, lkl_path);
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

int anyfs_disk_probe(AnyfsDisk* d, unsigned int part, char fstype[32],
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

int anyfs_disk_leave(AnyfsDisk* d, unsigned int part)
{
	if (!d)
		return -1;
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
