/* lkl_bridge.h — C wrapper for LKL operations (avoids C++ keyword conflicts) */
#ifndef LKL_BRIDGE_H
#define LKL_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Session lifecycle */
int lklb_kernel_init(uint32_t mem_mb);
int lklb_disk_add(const char* image_path, int readonly);
int lklb_disk_remove(int disk_id);
int lklb_mount(int disk_id, unsigned int part, const char* name,
	       char* mount_point_out, size_t mp_size, char* fstype_out,
	       size_t fs_size);
int lklb_umount(const char* name);
int lklb_partitions(int disk_id);

/* Directory operations */
typedef struct {
	char name[256];
	uint64_t size;
	uint64_t mtime;
	int is_dir;
} LklbDirEntry;

typedef struct lklb_dir lklb_dir_t;

lklb_dir_t* lklb_opendir(const char* path);
int lklb_readdir(lklb_dir_t* dir, LklbDirEntry* entry);
void lklb_closedir(lklb_dir_t* dir);

/* File operations */
int lklb_stat(const char* path, uint64_t* size, uint64_t* mtime, int* is_dir);
int lklb_open(const char* path);
long lklb_read(int fd, void* buf, unsigned long count);
void lklb_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* LKL_BRIDGE_H */
