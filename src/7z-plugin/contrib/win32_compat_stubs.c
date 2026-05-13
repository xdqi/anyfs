/* win32_compat_stubs.c — Stubs for symbols missing from Debian's mingw-w64
 *
 * msys2's libglib-2.0.a was built against a newer CRT that provides nftw32i64.
 * Debian's mingw-w64 only has nftw/nftw64. Provide a simple wrapper.
 */
#include <stddef.h>

/* nftw32i64 is just nftw with 32-bit ino/64-bit off on msys2.
 * On Debian mingw-w64, nftw already uses 64-bit off_t with
 * _FILE_OFFSET_BITS=64. We forward to the available nftw. */
typedef int (*nftw_fn_t)(const char*, const void*, int, int);
extern int nftw(const char* path, nftw_fn_t fn, int fd_limit, int flags);

int nftw32i64(const char* path, nftw_fn_t fn, int fd_limit, int flags)
{
	return nftw(path, fn, fd_limit, flags);
}
