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
#ifndef ANYFS_KINDPROBE_H
#define ANYFS_KINDPROBE_H

#include <stddef.h>
#include <stdint.h>

#include "anyfs_disk.h" /* AnyfsPartKind + anyfs_partkind_name */

#ifdef __cplusplus
extern "C" {
#endif

/* Classify based on a 64KB superblock buffer. */
AnyfsPartKind anyfs_kindprobe_buf(const void* buf, size_t len);

/* Open `/dev/<vdN>p<n>` (or `/dev/mapper/<name>`) via LKL, pread 64KB,
 * classify, close. Returns ANYFS_PART_KIND_FS on any I/O error. */
AnyfsPartKind anyfs_kindprobe_blkdev(const char* lkl_blkdev_path);

/* v2 enrichment: drive libblkid against a snapshot of the partition's
 * superblock area. `lkl_blkdev_path` is the LKL block device.
 * fstype / label / uuid buffers are populated (empty string if not
 * found). Returns 0 on success, non-zero on any I/O error (in which
 * case the buffers are left empty). Safe to call on container kinds —
 * libblkid simply won't recognise them as filesystems and returns
 * empty output.
 *
 * When libblkid is not compiled in (ANYFS_HAS_BLKID undefined), this
 * is a no-op that returns 0 with empty outputs. */
int anyfs_kindprobe_meta(const char* lkl_blkdev_path, char fstype[32],
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
int anyfs_partprobe_buf(const void* buf, size_t len, AnyfsInnerPart* out,
			int max);

/* Convenience: open the LKL block device, pread a sufficient prefix,
 * delegate to anyfs_partprobe_buf. Returns 0 on I/O failure or no
 * recognisable table. */
int anyfs_partprobe_blkdev(const char* lkl_blkdev_path, AnyfsInnerPart* out,
			   int max);

#ifdef __cplusplus
}
#endif

#endif
