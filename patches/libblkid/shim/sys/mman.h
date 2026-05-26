/* Stub <sys/mman.h> for mingw. libblkid's only mmap use is anonymous
 * private mappings used as read-buffer backing store (see probe.c
 * read_buffer). We emulate that with malloc/free. */
#ifndef SHIM_SYS_MMAN_H
#define SHIM_SYS_MMAN_H

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PROT_NONE  0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *) -1)

#define POSIX_MADV_NORMAL     0
#define POSIX_MADV_RANDOM     1
#define POSIX_MADV_SEQUENTIAL 2
#define POSIX_MADV_WILLNEED   3
#define POSIX_MADV_DONTNEED   4

/* libblkid only ever calls:
 *   mmap(NULL, len, PROT_*, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
 *   munmap(ptr, len);
 * We satisfy that with calloc/free. Any other use is a bug we'd want to
 * see, so we return MAP_FAILED. */
static inline void *shim_mmap(void *addr, size_t len, int prot, int flags,
			      int fd, long off)
{
	(void)addr; (void)prot;
	if (!(flags & MAP_ANONYMOUS) || fd != -1 || off != 0) {
		errno = ENOSYS;
		return MAP_FAILED;
	}
	void *p = calloc(1, len);
	if (!p) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return p;
}

static inline int shim_munmap(void *ptr, size_t len)
{
	(void)len;
	free(ptr);
	return 0;
}

/* libblkid's probe.c uses mprotect to make the read-buffer read-only
 * after the data has been pread()'d in. mingw-w64's runtime ships a
 * real mprotect() that wraps VirtualProtect, but our mmap is backed by
 * the C heap (calloc), not VirtualAlloc — so VirtualProtect on a heap
 * chunk corrupts heap metadata and later allocations page-fault.
 * Override with a no-op (the read-only marker is purely a debug aid;
 * libblkid logs failure and keeps going). */
static inline int shim_mprotect(void *addr, size_t len, int prot)
{
	(void)addr; (void)len; (void)prot;
	return 0;
}

#define mmap     shim_mmap
#define munmap   shim_munmap
#define mprotect shim_mprotect

#endif
