/*
 * anyfs_u8.h — the UTF-8 host layer.
 *
 * Inside anyfs every string is UTF-8. On Windows the CRT and Win32 "A"
 * functions read char strings in the ANSI code page, so a path or a line of
 * text outside that code page is mangled. Every host call that takes or
 * returns text therefore goes through this layer, which converts at the
 * boundary and calls the W (UTF-16) API. On other hosts each function is
 * the plain libc call.
 *
 * CLIs get the layer without source changes: meson force-includes
 * anyfs_u8_redirect.h (and links anyfs_u8_main.c for wmain). Core code is
 * linked into the Electron addon too and calls the file functions
 * explicitly; it is force-included with the output half only
 * (anyfs_u8_stdio.h).
 */
#ifndef ANYFS_U8_H
#define ANYFS_U8_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <io.h> /* no windows.h: this header is force-included everywhere */
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Length of the longest prefix of s[0..n) that does not end inside a UTF-8
 * sequence. Bytes that cannot start a sequence count as complete. */
size_t anyfs_u8_complete_prefix(const char* s, size_t n);

#ifdef _WIN32

/* Strict conversions (an invalid sequence is an error, never a '?'). Return
 * 0 and a malloc'd string, or -1 with errno set. */
int anyfs_u8_to_u16(const char* s, wchar_t** out);
int anyfs_u16_to_u8(const wchar_t* s, char** out);

int anyfs_u8_open(const char* path, int flags, ...);
FILE* anyfs_u8_fopen(const char* path, const char* mode);
int anyfs_u8_stat(const char* path, struct stat* st);
int anyfs_u8_access(const char* path, int mode);
int anyfs_u8_unlink(const char* path);
int anyfs_u8_mkstemp(char* tmpl);
/* Like getenv(): the string stays valid for the life of the process. */
char* anyfs_u8_getenv(const char* name);
/* A temporary file in %TEMP% that is removed when closed (scratch data such
 * as the probe spool). Returns an fd or -1. */
int anyfs_u8_tmpfile_fd(void);

/* stdout/stderr on a console: WriteConsoleW (an incomplete UTF-8 sequence
 * at the end of a write is held for the next one). Anything else: the
 * bytes unchanged. */
int anyfs_u8_vfprintf(FILE* f, const char* fmt, va_list ap);
int anyfs_u8_fprintf(FILE* f, const char* fmt, ...);
int anyfs_u8_vprintf(const char* fmt, va_list ap);
int anyfs_u8_printf(const char* fmt, ...);
int anyfs_u8_fputs(const char* s, FILE* f);
int anyfs_u8_puts(const char* s);
int anyfs_u8_fputc(int c, FILE* f);
int anyfs_u8_putchar(int c);
size_t anyfs_u8_fwrite(const void* p, size_t size, size_t n, FILE* f);
void anyfs_u8_perror(const char* s);

#else /* !_WIN32 */

#define anyfs_u8_open open
#define anyfs_u8_fopen fopen
#define anyfs_u8_stat stat
#define anyfs_u8_access access
#define anyfs_u8_unlink unlink
#define anyfs_u8_mkstemp mkstemp
#define anyfs_u8_getenv getenv

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_U8_H */
