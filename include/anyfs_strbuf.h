/*
 * anyfs_strbuf.h — Simple appendable string buffer.
 *
 * Growable output buffer — callers can defer error checks to the end
 * of a batch of writes (on allocation failure, .err is set and further
 * appends become no-ops).
 */
#ifndef ANYFS_STRBUF_H
#define ANYFS_STRBUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_STRBUF_H */
