#ifndef _COMPAT_GRP_H
#define _COMPAT_GRP_H
#include <stddef.h>
#ifndef _GID_T_DEFINED
typedef unsigned int gid_t;
#define _GID_T_DEFINED
#endif
struct group {
	char* gr_name;
	char* gr_passwd;
	gid_t gr_gid;
	char** gr_mem;
};
static inline struct group* getgrnam(const char* n)
{
	(void)n;
	return NULL;
}
static inline struct group* getgrgid(gid_t gid)
{
	(void)gid;
	return NULL;
}
static inline int getgrouplist(const char* user, gid_t group, gid_t* groups,
			       int* ngroups)
{
	(void)user;
	(void)group;
	if (*ngroups >= 1) {
		groups[0] = group;
		*ngroups = 1;
		return 1;
	}
	*ngroups = 1;
	return -1;
}
#endif
