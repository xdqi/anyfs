/* Stub <sys/ioctl.h> for mingw. libblkid's superblock probe code only
 * touches ioctl() when probing block devices (BLKGETSIZE64, BLKIOOPT,
 * etc.). For our use case the file is always regular, so the
 * S_ISBLK/S_ISCHR branches are dead. We provide an ioctl() stub that
 * always reports ENOSYS to libblkid; control flow falls back to lseek
 * SEEK_END for sizing. */
#ifndef SHIM_SYS_IOCTL_H
#define SHIM_SYS_IOCTL_H

#include <errno.h>

static inline int shim_ioctl(int fd, unsigned long req, ...)
{
	(void)fd; (void)req;
	errno = ENOSYS;
	return -1;
}
#define ioctl shim_ioctl

/* Linux block-device ioctls referenced by libblkid. Values do not
 * matter (our ioctl stub never reads them) — they only need to compile. */
#ifndef BLKGETSIZE64
# define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
# define BLKSSZGET    _IO(0x12, 104)
#endif
#ifndef BLKBSZGET
# define BLKBSZGET    _IOR(0x12, 112, size_t)
#endif
#ifndef BLKIOMIN
# define BLKIOMIN     _IO(0x12, 120)
#endif
#ifndef BLKIOOPT
# define BLKIOOPT     _IO(0x12, 121)
#endif
#ifndef BLKALIGNOFF
# define BLKALIGNOFF  _IO(0x12, 122)
#endif
#ifndef BLKPBSZGET
# define BLKPBSZGET   _IO(0x12, 123)
#endif
#ifndef BLKDISCARDZEROES
# define BLKDISCARDZEROES _IO(0x12, 124)
#endif
#ifndef BLKROGET
# define BLKROGET     _IO(0x12, 94)
#endif
#ifndef BLKRRPART
# define BLKRRPART    _IO(0x12, 95)
#endif
#ifndef BLKGETSIZE
# define BLKGETSIZE   _IO(0x12, 96)
#endif

/* Minimal _IO macros (we don't actually issue these) */
#ifndef _IO
# define _IO(t, n)        ((((t) & 0xff) << 8) | ((n) & 0xff))
# define _IOR(t, n, sz)   _IO(t, n)
# define _IOW(t, n, sz)   _IO(t, n)
# define _IOWR(t, n, sz)  _IO(t, n)
#endif

#endif
