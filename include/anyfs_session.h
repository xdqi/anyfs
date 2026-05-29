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

#include <stddef.h>
#include <stdint.h>

#include "anyfs.h"

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

typedef struct AnyfsDisk AnyfsDisk;

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
 * `flags` is the same bitmask as anyfs_disk_add (ANYFS_DISK_READONLY,
 *  ANYFS_BACKEND_*). On success **out is owned by the caller and must
 *  be released with anyfs_disk_close(). */
int anyfs_disk_open(const char* image_path, uint32_t flags, AnyfsDisk** out);

/* Idempotent; safe at atexit. */
void anyfs_disk_close(AnyfsDisk* d);

/* Cached superblock-probe fstype for the whole disk (no partition
 * table). Set during anyfs_disk_open; NULL if unknown. Caller does not
 * own the returned pointer. */
const char* anyfs_disk_whole_fstype_hint(const AnyfsDisk* d);

/* Cached dev_t for the whole-disk block device (e.g. for mknod).
 * Set during anyfs_disk_open; returns 0 if unknown. */
uint32_t anyfs_disk_whole_dev(const AnyfsDisk* d);

/* The display name = basename of the image (or anything the surface
 * pinned). Used in lspart-style output. */
const char* anyfs_disk_display(const AnyfsDisk* d);
int anyfs_disk_id(const AnyfsDisk* d);

/* Disk-level metadata: logical (virtual block-device) size from sysfs
 * and the outer partition-table flavour ("gpt", "dos", or ""). */
typedef struct {
	uint64_t logical_size; /* virtual block device size, in bytes */
	char pt_type[16];      /* "gpt", "dos", or "" if no PT detected */
} AnyfsDiskMeta;

/* Populate `*out` with the disk's logical size and PT flavour.
 * Returns 0 on success, negative on error. Safe to call any time
 * after anyfs_disk_open. */
int anyfs_disk_meta(AnyfsDisk* d, AnyfsDiskMeta* out);

/* Copy the disk's top-level partitions into `buf` (parent == -1).
 * Returns the number written (capped at buf_n). `got` is set to the
 * *full* count even if buf_n was too small. */
int anyfs_disk_list(AnyfsDisk* d, AnyfsPartInfo* buf, size_t buf_n,
		    size_t* got);

/* Total partition count (top-level only). */
size_t anyfs_disk_nparts(AnyfsDisk* d);

/* List children of a specific slot (v2). Pass parent_slot_id = -1 for
 * top-level — same as anyfs_disk_list. Children only exist after the
 * container slot has been successfully entered. */
int anyfs_disk_list_children(AnyfsDisk* d, int parent_slot_id,
			     AnyfsPartInfo* buf, size_t buf_n, size_t* got);

/* Enter a top-level partition by index. Equivalent to a one-segment
 * enter_path. Idempotent. Concurrent callers serialise per-slot.
 *
 * Returns 0 on success and writes the absolute LKL path into
 * `lkl_path` (e.g. "/lklmnt/anyfs_d0_p1") for KIND_FS. For container
 * kinds the call still succeeds (children become listable), but
 * `lkl_path[0] = '\0'` — there is no filesystem to mount at this
 * level. Callers that need a mount must then drill into a child. */
int anyfs_disk_enter(AnyfsDisk* d, unsigned int part, uint32_t flags,
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
int anyfs_disk_enter_path(AnyfsDisk* d, const struct AnyfsPathComp* comp,
			  size_t n_comp, uint32_t flags,
			  char lkl_path[ANYFS_LKL_PATH_MAX]);

/* Like anyfs_disk_enter_path, but the leaf is allowed to be a
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
 * keep using anyfs_disk_enter_path. */
int anyfs_disk_walk(AnyfsDisk* d, const struct AnyfsPathComp* comp,
		    size_t n_comp, uint32_t flags, int* leaf_slot_id_out,
		    char lkl_path[ANYFS_LKL_PATH_MAX]);

AnyfsPartState anyfs_disk_state(AnyfsDisk* d, unsigned int part);
AnyfsPartState anyfs_disk_state_slot(AnyfsDisk* d, int slot_id);

/* Reason string for FAILED partitions. Returns NULL when the partition
 * is in any non-FAILED state (NEW / MOUNTING / MOUNTED) and also when
 * `part` is out of range. Callers should NULL-guard. The returned
 * pointer is owned by the session and valid until anyfs_disk_close. */
const char* anyfs_disk_fail_reason(AnyfsDisk* d, unsigned int part);
const char* anyfs_disk_fail_reason_slot(AnyfsDisk* d, int slot_id);

/* Probe at the slot level. v2 fills fstype/label/uuid from cached
 * probe metadata (libblkid output, if available). Anything we don't
 * know stays as the empty string. */
int anyfs_disk_probe(AnyfsDisk* d, unsigned int part, char fstype[32],
		     char label[64], uint64_t* used);

/* Unmount a previously entered partition. Does not invalidate FAILED
 * cache. */
int anyfs_disk_leave(AnyfsDisk* d, unsigned int part);

#ifdef __cplusplus
}
#endif

#endif
