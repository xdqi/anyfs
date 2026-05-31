/*
 * kindprobe.h — partition-kind dispatch + libblkid enrichment
 *
 * v1: three superblock-magic checks dispatch AnyfsPartKind
 *     (LUKS / LVM_PV / nested PT / FS). Fast — one 64KB read.
 *
 * v2: same dispatch, plus an optional libblkid enrichment pass that
 *     fills fstype/label/UUID for KIND_FS rows. libblkid runs against
 *     a spooled host-side snapshot of the partition's first 2 MB so it
 *     never touches the host filesystem layout where the image lives.
 */
#ifndef ANYFS_ANYFS_PROBE_H
#define ANYFS_ANYFS_PROBE_H

#include <stddef.h>
#include <stdint.h>

#include "anyfs_session.h" /* AnyfsPartKind + anyfs_partkind_name */

#ifdef __cplusplus
extern "C" {
#endif

/* Classify based on a 64KB superblock buffer. */
AnyfsPartKind anyfs_probe_kind_buf(const void* buf, size_t len);

/* Open `/dev/<vdN>p<n>` (or `/dev/mapper/<name>`) via LKL, pread 64KB,
 * classify, close. Returns ANYFS_PART_KIND_FS on any I/O error. */
AnyfsPartKind anyfs_probe_kind_blkdev(const char* lkl_blkdev_path);

/* v2 enrichment: drive libblkid against a snapshot of the partition's
 * superblock area. `lkl_blkdev_path` is the LKL block device.
 * fstype / label / uuid buffers are populated (empty string if not
 * found). Returns 0 on success, non-zero on any I/O error (in which
 * case the buffers are left empty). Safe to call on container kinds —
 * libblkid simply won't recognise them as filesystems and returns
 * empty output. libblkid is always linked. */
int anyfs_probe_meta(const char* lkl_blkdev_path, char fstype[32],
		     char label[64], char uuid[40]);

/* Inner partition descriptor exposed to anyfs_disk's container walker.
 * `index` is 1-based; `start_bytes`/`size_bytes` are relative to the
 * parent block device. */
typedef struct AnyfsInnerPart {
	unsigned int index;
	uint64_t start_bytes;
	uint64_t size_bytes;
} AnyfsInnerPart;

/* Parse an MBR or GPT partition table from the start-of-device snapshot
 * `buf[0..len)`. Writes up to `max` entries to `out` and returns the
 * count, or 0 if no recognisable table is found. The caller passes a
 * buffer covering at least 64 KB (so GPT entry array fits). MBR extended
 * partitions are not chased — only primary entries are exposed. */
int anyfs_probe_pt_buf(const void* buf, size_t len, AnyfsInnerPart* out,
		       int max);

/* Convenience: open the LKL block device, pread a sufficient prefix,
 * delegate to anyfs_probe_pt_buf. Returns 0 on I/O failure or no
 * recognisable table. */
int anyfs_probe_pt_blkdev(const char* lkl_blkdev_path, AnyfsInnerPart* out,
			  int max);

/* Classify the partition-table flavour of a buffer covering the start
 * of a block device. Returns "gpt", "dos", or "" (empty = no table
 * detected). GPT is preferred when both signatures coexist
 * (protective-MBR + real GPT). */
const char* anyfs_probe_pttype_buf(const void* buf, size_t len);

/* Convenience: pread the prefix off the LKL block device and classify.
 * Returns the same strings as anyfs_probe_pttype_buf. */
const char* anyfs_probe_pttype_blkdev(const char* lkl_blkdev_path);

#ifdef __cplusplus
}
#endif

#endif
