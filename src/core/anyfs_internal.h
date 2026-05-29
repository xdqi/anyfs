/*
 * anyfs_internal.h — Internal types and helpers shared between
 *                     anyfs_session.c and anyfs_container.c.
 *
 * This header is NOT part of the public API; it lives in src/core/
 * and is only included by core implementation files.
 */
#ifndef ANYFS_INTERNAL_H
#define ANYFS_INTERNAL_H

#include "anyfs_session.h"

#include <pthread.h>
#include <stdint.h>

/* ── Slot constants ───────────────────────────────────────────── */

#define MAX_PARTS 128
#define ROOT_PARENT (-1)

/* Debug trace: set ANYFS_DEBUG=1 in the environment. */
int anyfs_dbg(void);

#define DBG(...)                                                               \
	do {                                                                   \
		if (anyfs_dbg())                                               \
			fprintf(stderr, "anyfs [%d]: ", getpid()),             \
			    fprintf(stderr, __VA_ARGS__);                      \
	} while (0)

/* ── Per-partition state ───────────────────────────────────────── */

typedef struct {
	int slot_id;	    /* equals its own index in parts[] */
	int parent_slot;    /* -1 = top-level under the disk */
	unsigned int index; /* partition number under parent (1-based) */
	uint64_t offset_bytes;
	uint64_t size_bytes;
	char ptype[40];
	char blkdev[80];     /* "/dev/vda1" or "/dev/mapper/<name>" */
	char dm_name[64];    /* set only for DM-backed slots */
	char sysfs_name[64]; /* set only for slots that own children */
	AnyfsPartKind kind;
	AnyfsPartState state;
	char* fail_reason;
	int children_loaded; /* 1 once sysfs-walked after dm setup */
	char fstype_cache[32];
	char label[64];
	char uuid[40];
	pthread_cond_t cv;
} PartSlot;

/* ── Session handle ────────────────────────────────────────────── */

struct AnyfsSession {
	int disk_id;
	uint32_t open_flags;
	char image_path[512];
	char display[256];
	char sysfs_name[64];	    /* "vda" */
	char whole_fstype_hint[32]; /* cached superblock probe result */
	uint32_t whole_dev;	    /* cached dev_t for whole-disk /dev node */
	pthread_mutex_t lock;
	PartSlot* parts; /* size = parts_cap */
	size_t n_parts;
	size_t parts_cap;
};

/* ── Helpers shared between session and container ──────────────── */

/* Mark a slot as FAILED with a strdup'd reason. */
void set_fail(PartSlot* p, const char* reason);

/* Allocate and initialise a new slot. Caller must hold the lock. */
int alloc_slot_locked(struct AnyfsSession* d, int parent_slot,
		      unsigned int index, uint64_t off, uint64_t size,
		      const char* blkdev, AnyfsPartKind kind);

#endif /* ANYFS_INTERNAL_H */
