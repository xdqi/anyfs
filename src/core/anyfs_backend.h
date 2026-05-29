#ifndef ANYFS_BACKEND_H
#define ANYFS_BACKEND_H

#include <lkl_host.h>
#include <stdint.h>

/*
 * Block backend operations interface.
 * Each backend (raw, gio, qemu) provides a static instance of this struct.
 */
struct anyfs_backend_ops {
	const char* name;
	int (*open)(const char* path, uint32_t flags,
		    struct lkl_disk* disk_out);
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

/* ── Disk-slot management (internal) ───────────────────────────── */

/* Per-disk slot tracked in g_disks[]. Shared between backends (which
 * allocate/close), kernel lifecycle (which drains on atexit), and the
 * session layer (which opens/closes sessions). */
struct disk_slot {
	int in_use;
	int disk_id;
	struct lkl_disk disk;
	const struct anyfs_backend_ops* backend;
};

extern struct disk_slot g_disks[];

/* Add / remove a block device from the LKL virtio-blk bus. These are
 * internal helpers called by the session layer during open/close; the
 * public surface is anyfs_session_open / anyfs_session_close. */
int anyfs_disk_add(const char* image_path, uint32_t flags);
int anyfs_disk_remove(int disk_id);

#endif /* ANYFS_BACKEND_H */
