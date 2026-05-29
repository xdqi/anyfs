/*
 * anyfs_container.c — Container-slot handling: LUKS credential
 *                      resolution + NESTED/LUKS/LVM_PV enter.
 */
#define _GNU_SOURCE
#include "anyfs_container.h"
#include "anyfs_internal.h"

#include "anyfs_dm.h"
#include "anyfs_probe.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
				/* Use fopen so this stays portable to mingw,
				 * where <fcntl.h> lacks O_CLOEXEC. CLOEXEC
				 * semantics are irrelevant here — we read the
				 * keyfile to a buffer and close immediately,
				 * not across a fork/exec. */
				FILE* fp = fopen(v, "rb");
				if (!fp) {
					snprintf(errstr, 160,
						 "keyfile=%s: open failed "
						 "(errno=%d)",
						 v, errno);
					free(dup);
					return -1;
				}
				size_t n = fread(out, 1, out_cap, fp);
				fclose(fp);
				if (n == 0) {
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

/* Enter a container slot: parse the inner partition table directly,
 * create one dm-linear per inner partition, append child slots. The
 * kernel won't auto-scan partitions on a dm device (GENHD_FL_NO_PART),
 * so we own the parse. Caller must NOT hold the lock. */
int enter_container_slot(struct AnyfsDisk* d, int slot_id, const char* query,
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
	/* DM v2 plumbing is not ready; refuse nested partition tables entirely
	 * until dm-linear creation + kindprobe recursion is validated. */
	if (kind == ANYFS_PART_KIND_NESTED_PARTITION_TABLE) {
		rc = -LKL_ENOSYS;
		goto fail_locked;
	}

	AnyfsInnerPart inners[MAX_PARTS];
	int n_inner = 0;
	if (kind == ANYFS_PART_KIND_NESTED_PARTITION_TABLE) {
		n_inner =
		    anyfs_probe_pt_blkdev(parent_blkdev, inners, MAX_PARTS);
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

fail_locked:
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
			AnyfsPartKind ck = anyfs_probe_kind_blkdev(cout);
			char fst[32] = "", lbl[64] = "", uid[40] = "";
			if (ck == ANYFS_PART_KIND_FS)
				(void)anyfs_probe_meta(cout, fst, lbl, uid);
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
