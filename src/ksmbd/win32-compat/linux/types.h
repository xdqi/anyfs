#ifndef _COMPAT_LINUX_TYPES_H
#define _COMPAT_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;
typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u64 __be64;

#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef _UID_T_DEFINED
typedef unsigned int uid_t;
#define _UID_T_DEFINED
#endif
#ifndef _GID_T_DEFINED
typedef unsigned int gid_t;
#define _GID_T_DEFINED
#endif

/* POSIX stubs for Win32 */
static inline int kill(int pid, int sig)
{
	(void)pid;
	(void)sig;
	return -1;
}
/* gethostname is in ws2_32 but winsock2.h causes header hell */
int __attribute__((stdcall)) gethostname(char* name, int namelen);
#endif
