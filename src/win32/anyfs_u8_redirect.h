/*
 * anyfs_u8_redirect.h — force-included (Windows only) into every source of
 * the CLIs: console output (anyfs_u8_stdio.h) plus file and environment
 * calls, all through the UTF-8 host layer. See anyfs_u8.h.
 */
#ifndef ANYFS_U8_REDIRECT_H
#define ANYFS_U8_REDIRECT_H

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "anyfs_u8_stdio.h"

#undef open
#undef fopen
#undef stat
#undef access
#undef unlink
#undef getenv
#undef mkstemp
#define open(...) anyfs_u8_open(__VA_ARGS__)
#define fopen(path, mode) anyfs_u8_fopen(path, mode)
#define stat(path, st) anyfs_u8_stat(path, st)
#define access(path, mode) anyfs_u8_access(path, mode)
#define unlink(path) anyfs_u8_unlink(path)
#define getenv(name) anyfs_u8_getenv(name)
#define mkstemp(tmpl) anyfs_u8_mkstemp(tmpl)

#endif /* ANYFS_U8_REDIRECT_H */
