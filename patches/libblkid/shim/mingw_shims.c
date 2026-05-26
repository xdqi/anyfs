/* mingw_shims.c — stubs for util-linux/libblkid symbols that mingw's
 * libc cannot satisfy. None of these are reached by the superblocks
 * probing call chain when libblkid is pointed at a regular file (which
 * is the only way kindprobe.c uses it). They exist purely so the
 * static archive can be linked; calling any of them at runtime would
 * be a logic bug, so we either no-op or return -1/ENOSYS.
 *
 * The symbols fall into four buckets:
 *  1. POSIX functions absent from mingw (link, fchmod, setenv, ...,
 *     getuid/geteuid/...).
 *  2. util-linux libcommon helpers we dropped because they pull in
 *     sysfs/openat/etc. that mingw lacks (canonicalize_path,
 *     blkdev_get_size, mkstemp_cloexec, ul_path_*, sysfs_*).
 *  3. POSIX file ops absent from mingw (dirfd, fstatat).
 *  4. major/minor/makedev macros (no <sys/sysmacros.h>).
 *
 * Anything that needs to function (mkstemp_cloexec is used by
 * blkid_save_cache via libcommon-style fileutils, but our entry point
 * never flushes the cache) is left as a tombstone. */

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>

/* ---- bucket 0: force all open()s to binary mode ----
 *
 * libblkid (and the rest of util-linux) was written for POSIX and calls
 * `open(path, O_RDONLY|O_CLOEXEC|O_NONBLOCK)` without O_BINARY. On mingw
 * the global `_fmode` defaults to _O_TEXT, so every read() through that
 * fd is run through msvcrt's text-mode translator. The translator does
 * two destructive things:
 *   - it collapses CR/LF pairs in the byte stream, returning short reads
 *   - it treats 0x1A (Ctrl-Z) as a logical EOF and stops at it
 * Both make disk-image bytes look corrupted to libblkid's superblock
 * probers. Symptom we hit: ext4/ext2 superblocks contain a 0x1A byte
 * inside the first 512 bytes of the SB region, so read() returns fewer
 * bytes than asked and probe.c:read_buffer() bails with
 * "read failed: Success" (errno=0, short read). The xfs SB has no 0x1A
 * in the same window and detects fine. That's why a previous run looked
 * like a partial heap fault but was actually CRT text-mode I/O eating
 * the magic.
 *
 * Force binary mode for the whole process. Done via a CRT constructor
 * so it runs before anything in libblkid (or anyfs-ksmbd.exe) opens a
 * file. The official mingw recipe — defining `int _CRT_fmode = _O_BINARY;`
 * — only works when the executable's CRT startup honours the symbol;
 * with -shared / when libblkid is consumed from an existing exe this is
 * fragile. _set_fmode() is unconditional and runs at our ctor time. */
static void __attribute__((constructor)) blkid_shim_force_binary(void)
{
	_set_fmode(_O_BINARY);
}

/* Belt-and-suspenders for callers that link statically and pin _CRT_fmode
 * before our ctor runs. Marked weak so it doesn't clash with anyfs's own. */
int _CRT_fmode __attribute__((weak)) = _O_BINARY;

/* ---- bucket 1: POSIX I/O & creds ---- */

int link(const char *oldpath, const char *newpath)
{
	(void)oldpath; (void)newpath;
	errno = ENOSYS;
	return -1;
}

int fchmod(int fd, unsigned int mode)
{
	(void)fd; (void)mode;
	return 0; /* No-op: Windows has no POSIX permission bits */
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (!name || !*name || strchr(name, '=')) {
		errno = EINVAL;
		return -1;
	}
	if (!overwrite && getenv(name))
		return 0;
	size_t nlen = strlen(name), vlen = value ? strlen(value) : 0;
	char *buf = (char *)malloc(nlen + vlen + 2);
	if (!buf) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(buf, name, nlen);
	buf[nlen] = '=';
	if (value)
		memcpy(buf + nlen + 1, value, vlen);
	buf[nlen + 1 + vlen] = '\0';
	int rc = _putenv(buf);
	free(buf);
	return rc;
}

/* Windows has no POSIX uid/gid. libblkid uses these only to detect
 * "running as root" so it can read /etc/blkid.tab. Return 0 (root)
 * consistently so all 4 functions return 0 and the suid check is a no-op. */
unsigned int getuid(void)  { return 0; }
unsigned int geteuid(void) { return 0; }
unsigned int getgid(void)  { return 0; }
unsigned int getegid(void) { return 0; }

/* nanosleep64 lives in mingw-w64's libpthread (winpthreads). When
 * libblkid is linked into the final anyfs-ksmbd.exe, that already
 * pulls in -lpthread, so we leave the symbol unresolved here and let
 * the host link satisfy it. To keep the smoke link simple, the
 * smoke.exe build adds -lpthread. */

int dirfd(void *dirp) { (void)dirp; errno = ENOTSUP; return -1; }

int fstatat(int dirfd, const char *pathname, void *statbuf, int flags)
{
	(void)dirfd; (void)pathname; (void)statbuf; (void)flags;
	errno = ENOSYS;
	return -1;
}

/* ---- bucket 4: major/minor/makedev ---- */

unsigned int major(unsigned long long dev)
{
	return (unsigned int)((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000);
}
unsigned int minor(unsigned long long dev)
{
	return (unsigned int)(dev & 0xff) | (unsigned int)((dev >> 12) & 0xffffff00);
}
unsigned long long makedev(unsigned int maj, unsigned int min)
{
	return ((unsigned long long)(min & 0xff)) |
	       ((unsigned long long)(maj & 0xfff) << 8) |
	       ((unsigned long long)(min & ~0xff) << 12) |
	       ((unsigned long long)(maj & ~0xfff) << 32);
}

/* ---- bucket 2: util-linux helpers we dropped ---- */

/* blkdev.c: never reached for regular files. */
int blkdev_get_sector_size(int fd, int *sector_size)
{
	(void)fd;
	if (sector_size) *sector_size = 512;
	return 0;
}
int blkdev_get_size(int fd, unsigned long long *bytes)
{
	(void)fd;
	if (bytes) *bytes = 0;
	return -1;
}

/* canonicalize.c: only used to resolve /dev paths for cache lookups. */
char *canonicalize_path(const char *path)
{
	(void)path;
	return NULL;
}
char *canonicalize_dm_name(const char *ptname)
{
	(void)ptname;
	return NULL;
}

/* fileutils.c: only used to write the persistent cache file. */
int mkstemp_cloexec(char *template_name)
{
	(void)template_name;
	errno = ENOSYS;
	return -1;
}

/* path.c / sysfs.c / procfs.c: sysfs walking for partition geometry.
 * Never reached for a regular file. */
struct path_cxt;
struct path_cxt *ul_new_sysfs_path(unsigned long long devno,
				   struct path_cxt *parent, const char *prefix)
{
	(void)devno; (void)parent; (void)prefix;
	return NULL;
}
void ul_unref_path(struct path_cxt *pc) { (void)pc; }
void *ul_path_opendir(struct path_cxt *pc, const char *path)
{
	(void)pc; (void)path;
	return NULL;
}
int ul_path_read_u32(struct path_cxt *pc, unsigned int *res, const char *path)
{
	(void)pc; (void)path;
	if (res) *res = 0;
	return -1;
}
int ul_path_read_u64(struct path_cxt *pc, unsigned long long *res,
		     const char *path)
{
	(void)pc; (void)path;
	if (res) *res = 0;
	return -1;
}
int ul_path_readf_u64(struct path_cxt *pc, unsigned long long *res,
		      const char *fmt, ...)
{
	(void)pc; (void)fmt;
	if (res) *res = 0;
	return -1;
}
int ul_path_read_string(struct path_cxt *pc, char **str, const char *path)
{
	(void)pc; (void)path;
	if (str) *str = NULL;
	return -1;
}

/* sysfs.c helpers used by partitions.c / devname.c. */
int sysfs_blkdev_is_partition_dirent(void *dir, void *dent, const char *parent)
{
	(void)dir; (void)dent; (void)parent;
	return 0;
}
int sysfs_devname_to_devno(const char *name, unsigned long long *devno)
{
	(void)name;
	if (devno) *devno = 0;
	return -1;
}
int __sysfs_devname_to_devno(const char *prefix, const char *name,
			     const char *parent, unsigned long long *devno)
{
	(void)prefix; (void)name; (void)parent;
	if (devno) *devno = 0;
	return -1;
}
int sysfs_devno_is_dm_private(unsigned long long devno, char **uuid)
{
	(void)devno;
	if (uuid) *uuid = NULL;
	return 0;
}
char *sysfs_devno_to_devpath(unsigned long long devno)
{
	(void)devno; return NULL;
}
char *sysfs_devno_to_wholedisk(unsigned long long devno, char *buf,
			       size_t bufsiz, unsigned long long *diskdevno)
{
	(void)devno; (void)buf; (void)bufsiz;
	if (diskdevno) *diskdevno = 0;
	return NULL;
}
char *sysfs_chrdev_devno_to_devname(unsigned long long devno, char *buf,
				    size_t bufsiz)
{
	(void)devno; (void)buf; (void)bufsiz;
	return NULL;
}
