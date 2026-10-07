/*
 * anyfs_name.h — file names on the way to JavaScript (internal)
 *
 * Kernel file names are bytes. The UI receives them in JSON strings, and
 * JavaScript strings are Unicode: bytes that are not UTF-8 would turn into
 * U+FFFD and the name could no longer be opened. anyfs_name_escape maps
 * every byte that is not part of strict UTF-8 to a private-use character
 * U+EF00 + byte (U+EF80..U+EFFF), and anyfs_name_unescape maps them back.
 * Characters already in that range are escaped byte by byte too, so the
 * round trip is exact for every byte string and the escaped form is always
 * valid UTF-8. Pure string logic, unit-tested without LKL.
 */
#ifndef ANYFS_NAME_H
#define ANYFS_NAME_H

#include <stddef.h>

/* Room for the escape of an n-byte string, NUL included. */
#define ANYFS_NAME_ESCAPE_MAX(n) (3 * (n) + 1)

/* Both return the output length (no NUL; out is NUL-terminated) or -1 if
 * it does not fit in cap bytes, in which case out holds "". */
int anyfs_name_escape(const char* in, char* out, size_t cap);
int anyfs_name_unescape(const char* in, char* out, size_t cap);

#endif /* ANYFS_NAME_H */
