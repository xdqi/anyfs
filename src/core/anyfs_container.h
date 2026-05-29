/*
 * anyfs_container.h — Container-slot handling (internal).
 *
 * enter_container_slot resolves NESTED / LUKS / LVM_PV partition
 * slots — parsing inner partition tables, creating dm-linear devices,
 * and appending child slots.  Called from the session layer's enter /
 * enter_path / walk paths.
 */
#ifndef ANYFS_CONTAINER_H
#define ANYFS_CONTAINER_H

#include <stdint.h>

struct AnyfsDisk;

/* Enter a container slot (NESTED / LUKS / LVM_PV).  On success the
 * slot transitions to MOUNTED and its children become listable.
 * `query` is the path-DSL query string (may be NULL).  Returns 0 on
 * success, negative on failure (slot is left in FAILED state). */
int enter_container_slot(struct AnyfsDisk* d, int slot_id, const char* query,
			 uint32_t flags);

#endif /* ANYFS_CONTAINER_H */
