/*
 * anyfs_disk_dump.h — Shared table printer.
 *
 * Same output for `anyfs-lspart` and the `.partitions` synthetic file.
 *
 * The dump functions write into a growable string buffer (AnyfsStrbuf)
 * rather than a FILE*, so callers don't need fmemopen/open_memstream
 * (which are POSIX-only — mingw doesn't have them).
 */
#ifndef ANYFS_DISK_DUMP_H
#define ANYFS_DISK_DUMP_H

#include "anyfs_disk.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Simple appendable string buffer. On allocation failure, .err is set
 * and further appends become no-ops, so callers can defer error checks
 * to the end of a batch of writes. */
typedef struct {
	char* buf;  /* NUL-terminated while .err == 0 */
	size_t len; /* bytes in buf, excluding NUL */
	size_t cap; /* allocation size */
	int err;    /* non-zero on OOM */
} AnyfsStrbuf;

void anyfs_strbuf_init(AnyfsStrbuf* sb);
void anyfs_strbuf_free(AnyfsStrbuf* sb);

/* Transfer ownership of the buffer to the caller; the strbuf is reset
 * to empty. If *len_out is non-NULL it receives the string length
 * (excluding NUL). Returns NULL if the buffer is in an error state. */
char* anyfs_strbuf_detach(AnyfsStrbuf* sb, size_t* len_out);

void anyfs_strbuf_printf(AnyfsStrbuf* sb, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Print the header line into sb. */
void anyfs_dump_header(AnyfsStrbuf* sb);

/* Print one disk's rows into sb. `disk_idx` is the positional 0-based
 * index the surface assigned (drives the `diskN/pM` PATH column). */
void anyfs_dump_disk(AnyfsStrbuf* sb, AnyfsDisk* d, int disk_idx);

#ifdef __cplusplus
}
#endif

#endif
