/*
 * anyfs_format.h — Shared table printer.
 *
 * Same output for `anyfs-lspart` and the `.partitions` synthetic file.
 *
 * The dump functions write into a growable string buffer (AnyfsStrbuf)
 * rather than a FILE*, so callers don't need fmemopen/open_memstream
 * (which are POSIX-only — mingw doesn't have them).
 */
#ifndef ANYFS_FORMAT_H
#define ANYFS_FORMAT_H

#include "anyfs_session.h"
#include "anyfs_strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Print the header line into sb. */
void anyfs_format_header(AnyfsStrbuf* sb);

/* Print one disk's rows into sb. `disk_idx` is the positional 0-based
 * index the surface assigned (drives the `diskN/pM` PATH column). */
void anyfs_format_disk(AnyfsStrbuf* sb, AnyfsDisk* d, int disk_idx);

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_FORMAT_H */
