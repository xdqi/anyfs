/* Stub <grp.h> for mingw. libblkid superblocks probing never resolves
 * a group; the header is included transitively from c.h. */
#ifndef SHIM_GRP_H
#define SHIM_GRP_H

#include <sys/types.h>

#ifndef _GID_T_DEFINED
typedef unsigned int gid_t;
#define _GID_T_DEFINED
#endif
#ifndef _UID_T_DEFINED
typedef unsigned int uid_t;
#define _UID_T_DEFINED
#endif

struct group {
	char *gr_name;
	char *gr_passwd;
	gid_t gr_gid;
	char **gr_mem;
};

static inline struct group *getgrnam(const char *name) { (void)name; return 0; }
static inline struct group *getgrgid(gid_t gid) { (void)gid; return 0; }

#endif
