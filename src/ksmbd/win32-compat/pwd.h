#ifndef _COMPAT_PWD_H
#define _COMPAT_PWD_H
#include <stddef.h>
#ifndef _UID_T_DEFINED
typedef unsigned int uid_t;
#define _UID_T_DEFINED
#endif
#ifndef _GID_T_DEFINED
typedef unsigned int gid_t;
#define _GID_T_DEFINED
#endif
struct passwd {
	char* pw_name;
	char* pw_passwd;
	uid_t pw_uid;
	gid_t pw_gid;
	char* pw_dir;
	char* pw_shell;
};

/*
 * Stub: Wine/mingw has no /etc/passwd. anyfs-ksmbd does NOT rely on
 * getpwnam for permission bypass — it stamps force_uid/force_gid = 0 on
 * every share programmatically in lkl_ksmbd.c (see finalize_in_memory_config).
 * If ksmbd-tools still calls getpwnam (e.g. for usm_add_new_user on a
 * non-guest principal), returning NULL is fine: that path falls back to
 * INVALID_UID. */
static inline struct passwd* getpwnam(const char* n)
{
	(void)n;
	return NULL;
}
static inline struct passwd* getpwuid(uid_t uid)
{
	(void)uid;
	return NULL;
}
static inline struct passwd* getpwent(void)
{
	return NULL;
}
static inline void setpwent(void)
{
}
static inline void endpwent(void)
{
}
static inline int getppid(void)
{
	return 1;
}
#endif
