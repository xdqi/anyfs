/*
 * anyfs_u8_stdio.h — force-included (Windows only) into code whose console
 * output must go through the UTF-8 host layer: printf and friends on
 * stdout/stderr become WriteConsoleW on a console. See anyfs_u8.h.
 *
 * Function-like macros, so only calls are rewritten. The real declarations
 * are included first; their include guards keep later includes from
 * declaring the macro names again.
 */
#ifndef ANYFS_U8_STDIO_H
#define ANYFS_U8_STDIO_H

#include <stdio.h>

#include "anyfs_u8.h"

#undef printf
#undef vprintf
#undef fprintf
#undef vfprintf
#undef puts
#undef fputs
#undef fputc
#undef putc
#undef putchar
#undef fwrite
#undef perror
#define printf(...) anyfs_u8_printf(__VA_ARGS__)
#define vprintf(fmt, ap) anyfs_u8_vprintf(fmt, ap)
#define fprintf(...) anyfs_u8_fprintf(__VA_ARGS__)
#define vfprintf(f, fmt, ap) anyfs_u8_vfprintf(f, fmt, ap)
#define puts(s) anyfs_u8_puts(s)
#define fputs(s, f) anyfs_u8_fputs(s, f)
#define fputc(c, f) anyfs_u8_fputc(c, f)
#define putc(c, f) anyfs_u8_fputc(c, f)
#define putchar(c) anyfs_u8_putchar(c)
#define fwrite(p, size, n, f) anyfs_u8_fwrite(p, size, n, f)
#define perror(s) anyfs_u8_perror(s)

#endif /* ANYFS_U8_STDIO_H */
