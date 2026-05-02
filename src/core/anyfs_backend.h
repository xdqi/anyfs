#ifndef ANYFS_BACKEND_H
#define ANYFS_BACKEND_H

#include <lkl_host.h>

/*
 * Block backend operations interface.
 * Each backend (raw, gio, qemu) provides a static instance of this struct.
 */
struct anyfs_backend_ops {
	const char* name;
	int (*open)(const char* path, int readonly, struct lkl_disk* disk_out);
	void (*close)(struct lkl_disk* disk);
};

/* Backend registry (populated at link time) */
#define ANYFS_MAX_BACKENDS 8

extern const struct anyfs_backend_ops* anyfs_backends[];
extern int anyfs_backend_count;

/* Register a backend (call from constructor or init) */
void anyfs_register_backend(const struct anyfs_backend_ops* ops);

/* Find backend by name */
const struct anyfs_backend_ops* anyfs_find_backend(const char* name);

#endif /* ANYFS_BACKEND_H */
