/*
 * sys/uio.h — no-op shim for mingw builds.
 *
 * QEMU's <qemu/osdep.h> defines struct iovec on Windows. LKL's lkl_host.h
 * would re-define it unless either LKL_HOST_CONFIG_POSIX or __MSYS__ is
 * set, in which case it #includes <sys/uio.h> instead.
 * src/core/qemu_blk_backend.c #defines __MSYS__ around its lkl includes to
 * avoid the collision; this header satisfies the resulting <sys/uio.h> include
 * with nothing — the iovec from QEMU is already visible.
 *
 * Only loaded by sources that #define __MSYS__ themselves.
 */
