/*
 * anyfs_disk.h — Multi-partition session layer.
 *
 * Wraps anyfs.h with:
 *   - lazy per-partition mounting (NEW → MOUNTING → MOUNTED | FAILED)
 *   - sysfs-based partition discovery
 *   - kind probe (LUKS / LVM_PV / nested PT / FS)
 *   - serialised concurrent enters
 *   - (v2) container recursion via dm-linear / dm-crypt
 *
 * The session is internally tree-shaped: top-level partitions of the
 * outer disk and any children materialised by entering a container
 * (NESTED partition table, LVM PV, LUKS volume) share a single flat
 * slot array. Each slot carries a `parent` handle: -1 for top-level
 * slots, otherwise the slot id of the container that produced it.
 *
 * v2 implements NESTED partition tables end-to-end. LVM_PV and LUKS
 * are recognised by kindprobe but enter fails with a clear FAILED
 * reason pointing at v3 work (LVM metadata parser / libcryptsetup).
 */
#ifndef ANYFS_SESSION_H
#define ANYFS_SESSION_H

#include <lkl.h>
#include <stddef.h>
#include <stdint.h>

/* ── Shared limits ──────────────────────────────────────────── */

#define ANYFS_MAX_DISKS 16  /* max disk images per session */
#define ANYFS_MAX_SHARES 32 /* max --share specs per server frontend */
#define ANYFS_LKL_PATH_MAX                                                     \
	64 /* max LKL mount point path length                                  \
	      (e.g. /lklmnt/anyfs_d0_p1) */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare AnyfsPathComp to avoid pulling in path_dsl.h here. */
struct AnyfsPathComp;

typedef struct AnyfsSession AnyfsSession;

typedef enum {
	ANYFS_PART_KIND_FS = 0, /* plain filesystem; enterable */
	ANYFS_PART_KIND_LVM_PV, /* container */
	ANYFS_PART_KIND_LUKS,	/* container */
	ANYFS_PART_KIND_NESTED_PARTITION_TABLE, /* container */
	ANYFS_PART_KIND_UNKNOWN,
} AnyfsPartKind;

const char* anyfs_partkind_name(AnyfsPartKind k);

typedef struct {
	int slot_id;	    /* opaque stable handle for this slot */
	int parent;	    /* -1 = root, else slot_id of parent */
	unsigned int index; /* partition number under its parent */
	uint64_t offset_bytes;
	uint64_t size_bytes;
	char ptype[40]; /* MBR "0x83" / GPT type GUID; "?" if unknown */
	AnyfsPartKind kind;
	/* v2: optional probe metadata. Empty string means "unknown". */
	char fstype[32];
	char label[64];
	char uuid[40];
} AnyfsPartInfo;

typedef enum {
	ANYFS_PART_NEW = 0,
	ANYFS_PART_MOUNTING,
	ANYFS_PART_MOUNTED, /* leaf FS: mounted at /lklmnt; container: children
			       materialised */
	ANYFS_PART_FAILED,
} AnyfsPartState;

/* Open a disk image as a multi-partition session.
 * Internally calls anyfs_disk_add() and resolves the sysfs name.
 * `flags` is the same bitmask as anyfs_disk_add (ANYFS_SESSION_READONLY,
 *  ANYFS_BACKEND_*). On success **out is owned by the caller and must
 *  be released with anyfs_session_close(). */
int anyfs_session_open(const char* image_path, uint32_t flags,
		       AnyfsSession** out);

/* Idempotent; safe at atexit. */
void anyfs_session_close(AnyfsSession* d);

/* The display name = basename of the image (or anything the surface
 * pinned). Used in lspart-style output. */
const char* anyfs_session_display(const AnyfsSession* d);
int anyfs_session_id(const AnyfsSession* d);

/* Disk-level metadata: logical (virtual block-device) size from sysfs
 * and the outer partition-table flavour ("gpt", "dos", or ""). */
typedef struct {
	uint64_t logical_size; /* virtual block device size, in bytes */
	char pt_type[16];      /* "gpt", "dos", or "" if no PT detected */
} AnyfsSessionMeta;

/* Populate `*out` with the disk's logical size and PT flavour.
 * Returns 0 on success, negative on error. Safe to call any time
 * after anyfs_session_open. */
int anyfs_session_meta(AnyfsSession* d, AnyfsSessionMeta* out);

/* Copy partitions whose parent matches `parent_slot_id` into `buf`.
 * Pass -1 for top-level. Returns the number written (capped at buf_n).
 * `got` is set to the *full* count even if buf_n was too small. */
int anyfs_session_list(AnyfsSession* d, int parent_slot_id, AnyfsPartInfo* buf,
		       size_t buf_n, size_t* got);

/* Count partitions whose parent matches `parent_slot_id`.
 * Pass -1 for top-level. */
size_t anyfs_session_count(AnyfsSession* d, int parent_slot_id);

/* Enter a partition (or the whole disk). Idempotent. Concurrent callers
 * serialise per-slot.
 *
 *   part = 0  — whole disk, no partition table. Mounts the entire block
 *               device as a single filesystem using cached dev_t + fstype
 *               hint from anyfs_session_open. Returns 0 and writes the LKL
 *               mount path into lkl_path (e.g. "/lklmnt/anyfs_d0_whole").
 *   part >= 1 — top-level partition by index. Equivalent to a one-segment
 *               enter_path.
 *
 * Returns 0 on success. For KIND_FS the LKL path is written to lkl_path.
 * For container kinds the call still succeeds (children become listable),
 * but lkl_path[0] = '\0'. Callers that need a mount must drill into a
 * child. */
int anyfs_session_enter(AnyfsSession* d, unsigned int part, uint32_t flags,
			char lkl_path[ANYFS_LKL_PATH_MAX]);

/* Walk a chain of partition components (the parsed path-DSL output).
 * Walks the segments left to right: each non-final segment must
 * resolve to a container; the final segment must resolve to KIND_FS,
 * at which point it is mounted. Containers along the way are entered
 * (which materialises their children) so subsequent segments resolve.
 *
 * Returns 0 + writes mount_path on success; negative on failure.
 * `comp` must point at `n_comp` AnyfsPathComp entries (see path_dsl.h).
 * The query field of each comp (if non-NULL) is parsed for kind-
 * specific options (e.g. LUKS keyref/keyfile/keyfd/key). */
int anyfs_session_enter_path(AnyfsSession* d, const struct AnyfsPathComp* comp,
			     size_t n_comp, uint32_t flags,
			     char lkl_path[ANYFS_LKL_PATH_MAX]);

/* Like anyfs_session_enter_path, but the leaf is allowed to be a
 * container (in which case it is materialised so its children become
 * listable, and lkl_path[0] is set to '\0'). When the leaf is KIND_FS
 * the partition is mounted and lkl_path receives the mount path.
 *
 * On success returns 0 and sets *leaf_slot_id_out (if non-NULL) to the
 * slot id of the resolved leaf. Negative on any failure along the
 * walk / mount.
 *
 * Surfaces that need both modes (FUSE Mode B opendir) use this;
 * surfaces that strictly want a mounted FS (ksmbd, nfsd, FUSE Mode A)
 * keep using anyfs_session_enter_path. */
int anyfs_session_walk(AnyfsSession* d, const struct AnyfsPathComp* comp,
		       size_t n_comp, uint32_t flags, int* leaf_slot_id_out,
		       char lkl_path[ANYFS_LKL_PATH_MAX]);

AnyfsPartState anyfs_session_state(AnyfsSession* d, unsigned int part);
AnyfsPartState anyfs_session_state_slot(AnyfsSession* d, int slot_id);

/* Reason string for FAILED partitions. Returns NULL when the partition
 * is in any non-FAILED state (NEW / MOUNTING / MOUNTED) and also when
 * `part` is out of range. Callers should NULL-guard. The returned
 * pointer is owned by the session and valid until anyfs_session_close. */
const char* anyfs_session_fail_reason(AnyfsSession* d, unsigned int part);
const char* anyfs_session_fail_reason_slot(AnyfsSession* d, int slot_id);

/* Probe at the slot level. v2 fills fstype/label/uuid from cached
 * probe metadata (libblkid output, if available). Anything we don't
 * know stays as the empty string. */
int anyfs_session_probe(AnyfsSession* d, unsigned int part, char fstype[32],
			char label[64], uint64_t* used);

/* Unmount a previously entered partition. Does not invalidate FAILED
 * cache. */
int anyfs_session_leave(AnyfsSession* d, unsigned int part);

#ifdef __cplusplus
}
#endif

#endif
