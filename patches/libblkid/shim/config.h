/* Hand-rolled config.h for libblkid cross-built against mingw64.
 * Mirrors the wasm build's config.h but turns off everything that
 * pulls in Linux-only headers or syscalls. */
#ifndef UL_CONFIG_H
#define UL_CONFIG_H

/* Package metadata */
#define PACKAGE "util-linux"
#define PACKAGE_NAME "util-linux"
#define PACKAGE_VERSION "2.41"
#define PACKAGE_STRING "util-linux 2.41"
#define PACKAGE_BUGREPORT "kzak@redhat.com"
#define LIBBLKID_DATE "26-May-2026"
#define LIBBLKID_VERSION "2.41.0"

/* Locale dirs (unused, but referenced) */
#define LOCALEDIR "/usr/share/locale"

/* Standard headers that mingw has */
#define HAVE_FCNTL_H 1
#define HAVE_ERRNO_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_DIRENT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_GETOPT_H 1
#define HAVE_CTYPE_H 1
#define HAVE_TIME_H 1

/* Headers mingw lacks (these are intentionally NOT defined):
 * HAVE_SYS_IOCTL_H, HAVE_SYS_MOUNT_H, HAVE_SYS_SYSMACROS_H,
 * HAVE_SYS_RANDOM_H, HAVE_SYS_MMAN_H, HAVE_SYS_TIME_H,
 * HAVE_ENDIAN_H, HAVE_BYTESWAP_H, HAVE_PWD_H, HAVE_GRP_H,
 * HAVE_ERR_H, HAVE_DLFCN_H, HAVE_FNMATCH_H, HAVE_LANGINFO_H,
 * HAVE_LINUX_*, HAVE_CRYPT_H, etc. */

/* Endianness (mingw x86_64 is little-endian) */
#define HAVE_DECL_BSWAP_16 0
#define HAVE_DECL_BSWAP_32 0
#define HAVE_DECL_BSWAP_64 0

#define HAVE_DECL_BE16TOH 0
#define HAVE_DECL_BE32TOH 0
#define HAVE_DECL_BE64TOH 0
#define HAVE_DECL_LE16TOH 0
#define HAVE_DECL_LE32TOH 0
#define HAVE_DECL_LE64TOH 0

/* Common libc functions mingw provides */
#define HAVE_FSEEKO 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRNDUP 1
#define HAVE_STRNLEN 1
#define HAVE_USLEEP 1
#define HAVE_NANOSLEEP 1
#define HAVE_MEMPCPY 0
#define HAVE_STRSEP 0
#define HAVE_STPNCPY 1
#define HAVE_STPCPY 1

/* getline is in mingw runtime since gcc 4.8+ */
#define HAVE_GETLINE 1
#define HAVE_GETDELIM 1

/* Things mingw lacks */
/* HAVE_EXPLICIT_BZERO undefined -- use our own shim */
/* HAVE_EACCESS undefined */
/* HAVE_MKOSTEMP undefined */
/* HAVE_POSIX_FADVISE undefined */
/* HAVE_FUTIMENS undefined */
/* HAVE_OPENAT undefined */
/* HAVE_FSTATAT undefined */
/* HAVE_DIRFD undefined */
/* HAVE_UNLINKAT undefined */
/* HAVE_FALLOCATE undefined */
/* HAVE_FDATASYNC undefined */
/* HAVE_FSYNC defined (mingw has it) */
#define HAVE_FSYNC 1

#define HAVE_FNMATCH 0

/* Disable NLS */
/* HAVE_NL_LANGINFO undefined */
/* HAVE_LANGINFO_CODESET undefined */

/* For struct stat.st_blksize */
/* HAVE_STRUCT_STAT_ST_BLKSIZE undefined - mingw stat lacks it */

/* On Windows time_t is 64-bit by default with mingw-w64 */
#define HAVE_CLOCK_GETTIME 0
/* mingw _CRT_VERSION provides timespec but not all */

/* No openpty or pty */

/* tls keyword */
#define HAVE_TLS 1

#define HAVE_DECL_ENVIRON 1
#define HAVE_ENVIRON_DECL 1

/* Disable LINUX-only features */
/* (no defines needed; absence is the off-switch) */

/* loff_t / off64_t — mingw uses _off64_t */
/* leave undefined */

/* Internal define to gate Linux-only blocks */
#define BLKID_NO_LINUX 1

/* mingw's sysconf doesn't define _SC_HOST_NAME_MAX. Use HOST_NAME_MAX
 * directly via c.h's static-inline fallback (we set it to 256). */
#ifndef _SC_HOST_NAME_MAX
# define _SC_HOST_NAME_MAX 180
#endif
#ifndef HOST_NAME_MAX
# define HOST_NAME_MAX 256
#endif

/* mingw lacks PATH_MAX in <limits.h>; pull it from <stdlib.h>'s _MAX_PATH. */
#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

/* Pretend to have sys/file.h (mingw has it, but no flock — we don't call it). */
#define HAVE_SYS_FILE_H 1

/* nanosleep is provided by mingw's pthread runtime headers (time.h). */

/* Mingw provides setlocale; declare HAVE_SETLOCALE for safety. */
#define HAVE_SETLOCALE 1

/* mingw lacks suseconds_t (POSIX). */
#ifndef _SUSECONDS_T_DEFINED
typedef long suseconds_t;
#define _SUSECONDS_T_DEFINED
#endif

/* mingw lacks O_NONBLOCK (no fcntl async I/O). libblkid passes it to
 * open(); on a regular file it's harmless to map to 0. */
#ifndef O_NONBLOCK
# define O_NONBLOCK 0
#endif

/* mingw lacks O_CLOEXEC. mingw's CRT honors _O_NOINHERIT instead, but
 * regular fds inherited into child processes don't matter for us
 * (we don't spawn). Map to 0. */
#ifndef O_CLOEXEC
# define O_CLOEXEC 0
#endif

/* mingw open() doesn't know S_IRWXU & friends as the umask family; the
 * <sys/stat.h> Posix constants are provided. Nothing to do. */

/* loff_t — used by some helpers. */
#ifndef _LOFF_T_DEFINED
typedef long long loff_t;
#define _LOFF_T_DEFINED
#endif

/* blkid format strings use GNU extensions %m (errno) and %z (size_t).
 * Mingw's printf goes through msvcrt by default and doesn't understand
 * either. We disable -Werror but warnings are noisy — silence by
 * pretending mingw uses gnu_printf. */
#ifndef __USE_MINGW_ANSI_STDIO
# define __USE_MINGW_ANSI_STDIO 1
#endif

/* Some superblock probers reference htobe16 etc. mingw lacks endian.h. */

/* mingw libc lacks strsep(3). Provide a tiny shim. */
#ifndef HAVE_STRSEP_DECL
#define HAVE_STRSEP_DECL 1
#include <string.h>
static inline char *strsep(char **stringp, const char *delim)
{
	char *p, *s;
	if ((s = *stringp) == NULL)
		return NULL;
	for (p = s; *p; p++) {
		const char *d;
		for (d = delim; *d; d++) {
			if (*p == *d) {
				*p = '\0';
				*stringp = p + 1;
				return s;
			}
		}
	}
	*stringp = NULL;
	return s;
}
#endif

/* mingw lacks pread/pwrite (msvcrt only). util-linux uses pread64 in
 * many places. Mingw-w64 has pread() declared in unistd.h since 2018+ */
/* mingw libc lacks fsync? Actually mingw provides _commit() but no
 * fsync(). util-linux save.c uses fsync. Provide a wrapper. */
#include <io.h>
#ifndef HAVE_FSYNC_DECL
# define HAVE_FSYNC_DECL 1
static inline int fsync(int fd) { return _commit(fd); }
#endif

/* mingw lacks loff_t/off64_t-related helpers but blkid superblocks
 * mostly use off_t with _FILE_OFFSET_BITS=64. */

/* S_ISUID, S_ISGID, S_ISVTX are POSIX but mingw doesn't define them
 * (Windows doesn't carry these mode bits). Define to 0 — they're only
 * used by xstrmode for octal mode printing which we never call. */
#ifndef S_ISUID
# define S_ISUID 0
#endif
#ifndef S_ISGID
# define S_ISGID 0
#endif
#ifndef S_ISVTX
# define S_ISVTX 0
#endif
#ifndef S_IRWXU
# define S_IRWXU 0700
#endif
#ifndef S_IRWXG
# define S_IRWXG 0070
#endif
#ifndef S_IRWXO
# define S_IRWXO 0007
#endif

/* mingw's open() doesn't know fstatat/openat; AT_* constants only used
 * by devno.c's scan_dir which our call chain never reaches at runtime.
 * Define so it compiles. */
#ifndef AT_SYMLINK_NOFOLLOW
# define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_FDCWD
# define AT_FDCWD (-100)
#endif

/* mingw mkdir is single-arg (Windows ignores mode). save.c's
 * blkid_flush_cache calls mkdir(path, mode) but that path is never
 * exercised by us. Override with a 2-arg shim that drops the mode. */
#define mkdir(p, m) mkdir(p)

#endif /* UL_CONFIG_H */
