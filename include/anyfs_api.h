#ifndef ANYFS_API_H
#define ANYFS_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef ANYFS_CORE_BUILDING
#define ANYFS_API __declspec(dllexport)
#else
#define ANYFS_API __declspec(dllimport)
#endif
#define ANYFS_CALL __cdecl
#else
#define ANYFS_API __attribute__((visibility("default")))
#define ANYFS_CALL
#endif

/* Opaque handles */
typedef struct AnyfsContext AnyfsContext;
typedef struct AnyfsMount AnyfsMount;
typedef struct AnyfsDir AnyfsDir;
typedef int64_t anyfs_fd_t;

/* Error codes */
#define ANYFS_OK 0
#define ANYFS_ERR_NOMEM (-1)
#define ANYFS_ERR_IO (-2)
#define ANYFS_ERR_INVAL (-3)
#define ANYFS_ERR_NOENT (-4)
#define ANYFS_ERR_NOTDIR (-5)
#define ANYFS_ERR_BUSY (-6)
#define ANYFS_ERR_NOSYS (-7)
#define ANYFS_ERR_FORMAT (-8)

/* Image open flags */
#define ANYFS_OPEN_READONLY 1
#define ANYFS_OPEN_GIO 2 /* Use GIO backend (cross-platform, sync) */
#define ANYFS_OPEN_GIO_ASYNC                                                   \
	4		 /* Use GIO async backend (GMainLoop dispatch)         \
			  */
#define ANYFS_OPEN_AIO 8 /* Use Linux AIO + epoll backend (true async) */

/* Init / destroy */
ANYFS_API int32_t ANYFS_CALL anyfs_init(AnyfsContext** ctx_out);
ANYFS_API void ANYFS_CALL anyfs_destroy(AnyfsContext* ctx);

/* Image operations */
ANYFS_API int32_t ANYFS_CALL anyfs_open_image(AnyfsContext* ctx,
					      const char* image_path,
					      uint32_t flags);

ANYFS_API int32_t ANYFS_CALL anyfs_mount(AnyfsContext* ctx, const char* fs_type,
					 uint32_t part_index,
					 AnyfsMount** mount_out);

ANYFS_API int32_t ANYFS_CALL anyfs_umount(AnyfsMount* mnt);

/* File operations */
ANYFS_API anyfs_fd_t ANYFS_CALL anyfs_open(AnyfsMount* mnt, const char* path,
					   uint32_t flags);

ANYFS_API int64_t ANYFS_CALL anyfs_read(AnyfsMount* mnt, anyfs_fd_t fd,
					void* buf, uint64_t count);

ANYFS_API int32_t ANYFS_CALL anyfs_close(AnyfsMount* mnt, anyfs_fd_t fd);

/* Directory operations */
typedef struct {
	uint8_t type; /* DT_REG=8, DT_DIR=4, DT_LNK=10 */
	uint8_t _pad[7];
	uint64_t inode;
	uint64_t size;
	char name[256]; /* UTF-8, NUL-terminated */
} AnyfsEntry;

ANYFS_API AnyfsDir* ANYFS_CALL anyfs_opendir(AnyfsMount* mnt, const char* path);
ANYFS_API int32_t ANYFS_CALL anyfs_readdir(AnyfsDir* dir,
					   AnyfsEntry* entry_out);
ANYFS_API int32_t ANYFS_CALL anyfs_closedir(AnyfsDir* dir);

#endif /* ANYFS_API_H */
