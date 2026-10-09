/*
 * anyfs_backend.c — Backend registry + disk-slot management (internal)
 */
#define _GNU_SOURCE
#include "anyfs_backend.h"
#include "anyfs.h"
#include "anyfs_session.h"
#include "raw_backend.h"
#ifdef ANYFS_HAS_GIO
#include "gio_backend.h"
#endif
#ifdef ANYFS_HAS_QEMU
#include "qemu_backend.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/stat.h>
#endif

#include "anyfs_u8.h"

/* ── Backend registry ──────────────────────────────────────────── */

const struct anyfs_backend_ops* anyfs_backends[ANYFS_MAX_BACKENDS];
int anyfs_backend_count;

void anyfs_register_backend(const struct anyfs_backend_ops* ops)
{
	if (anyfs_backend_count < ANYFS_MAX_BACKENDS)
		anyfs_backends[anyfs_backend_count++] = ops;
}

const struct anyfs_backend_ops* anyfs_find_backend(const char* name)
{
	for (int i = 0; i < anyfs_backend_count; i++)
		if (strcmp(anyfs_backends[i]->name, name) == 0)
			return anyfs_backends[i];
	return NULL;
}

/* ── Disk-slot array ───────────────────────────────────────────── */

struct disk_slot g_disks[ANYFS_MAX_DISKS];

/* ── Disk management ──────────────────────────────────────────── */

int anyfs_disk_add(const char* image_path, uint32_t flags)
{
	if (!image_path)
		return -1;

	/* Find free slot */
	int slot = -1;
	for (int i = 0; i < ANYFS_MAX_DISKS; i++) {
		if (!g_disks[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -1;

	/* Select backend */
	const struct anyfs_backend_ops* ops = NULL;
#ifdef ANYFS_HAS_QEMU
	if (flags & ANYFS_BACKEND_QEMU)
		ops = &qemu_backend_ops;
#endif
#ifdef ANYFS_HAS_GIO
	if (!ops && (flags & ANYFS_BACKEND_GIO))
		ops = &gio_backend_ops;
#endif
	if (!ops && (flags & ANYFS_BACKEND_RAW))
		ops = &raw_backend_ops;

	/* ANYFS_BACKEND=raw|qemu picks the backend when the caller didn't,
	 * so the CLI tools and the addon can be tested on each. */
	const char* env = anyfs_u8_getenv("ANYFS_BACKEND");
	if (!ops && env && strcmp(env, "raw") == 0)
		ops = &raw_backend_ops;
#ifdef ANYFS_HAS_QEMU
	if (!ops && env && strcmp(env, "qemu") == 0)
		ops = &qemu_backend_ops;
#endif

#if defined(__APPLE__) && defined(ANYFS_HAS_QEMU)
	/* QEMU's file driver refuses anything but a regular file, and its
	 * host_device driver needs IOKit, which the macOS build lacks: open a
	 * disk (/dev/diskN, /dev/rdiskN) with the raw backend. */
	struct stat st;
	if (!ops && stat(image_path, &st) == 0 &&
	    (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)))
		ops = &raw_backend_ops;
#endif
#if defined(_WIN32) && defined(ANYFS_HAS_QEMU)
	/* QEMU's win32 file driver only knows \\.\PhysicalDriveN and \\.\X:
	 * (sized as the whole disk). A partition (\\.\HarddiskNPartitionK)
	 * becomes drive "H:", fails to size, and a volume gets its disk's size:
	 * open the rest of the device namespace with the raw backend. */
	if (!ops && (strncmp(image_path, "\\\\.\\", 4) == 0 ||
		     strncmp(image_path, "//./", 4) == 0) &&
	    _strnicmp(image_path + 4, "PhysicalDrive", 13) != 0)
		ops = &raw_backend_ops;
#endif

	/* Auto-detect: prefer QEMU if available, else raw */
	if (!ops) {
#ifdef ANYFS_HAS_QEMU
		ops = &qemu_backend_ops;
#else
		ops = &raw_backend_ops;
#endif
	}

	struct lkl_disk disk;
	int ret = ops->open(image_path, flags, &disk);
	if (ret < 0)
		return -1;

	int disk_id = lkl_disk_add(&disk);
	if (disk_id < 0) {
		ops->close(&disk);
		return -1;
	}

	g_disks[slot].in_use = 1;
	g_disks[slot].disk_id = disk_id;
	g_disks[slot].disk = disk;
	g_disks[slot].backend = ops;
	return disk_id;
}

int anyfs_disk_remove(int disk_id)
{
	for (int i = 0; i < ANYFS_MAX_DISKS; i++) {
		if (g_disks[i].in_use && g_disks[i].disk_id == disk_id) {
			lkl_disk_remove(g_disks[i].disk);
			g_disks[i].backend->close(&g_disks[i].disk);
			g_disks[i].in_use = 0;
			return 0;
		}
	}
	return -1;
}
