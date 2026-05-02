/*
 * anyfs.c — Minimal LKL filesystem access: kernel init + disk management
 */
#define _GNU_SOURCE
#include "anyfs.h"
#include "anyfs_backend.h"
#include "raw_blk_backend.h"
#ifdef ANYFS_HAS_GIO
#include "gio_blk_backend.h"
#endif
#ifdef ANYFS_HAS_QEMU
#include "qemu_blk_backend.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal state ────────────────────────────────────────────── */

/* Backend registry */
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

#define MAX_DISKS 16

struct disk_slot {
	int in_use;
	int disk_id;
	struct lkl_disk disk;
	const struct anyfs_backend_ops* backend;
};

static struct disk_slot g_disks[MAX_DISKS];
static int g_kernel_started;

/* ── Kernel lifecycle ─────────────────────────────────────────── */

int anyfs_kernel_init(const AnyfsKernelOpts* opts)
{
	if (g_kernel_started)
		return 0;

	uint32_t mem_mb = 64;
	uint32_t loglevel = 0;
	if (opts) {
		if (opts->mem_mb)
			mem_mb = opts->mem_mb;
		loglevel = opts->loglevel;
	}

	/* Register backends */
	anyfs_register_backend(&raw_backend_ops);
#ifdef ANYFS_HAS_GIO
	anyfs_register_backend(&gio_backend_ops);
#endif
#ifdef ANYFS_HAS_QEMU
	anyfs_register_backend(&qemu_backend_ops);
#endif

	int ret = lkl_init(&lkl_host_ops);
	if (ret)
		return -1;

	char boot_args[128];
	snprintf(boot_args, sizeof(boot_args), "mem=%uM loglevel=%u", mem_mb,
		 loglevel);
	ret = lkl_start_kernel(boot_args);
	if (ret)
		return -1;

	g_kernel_started = 1;
	return 0;
}

void anyfs_kernel_halt(void)
{
	if (!g_kernel_started)
		return;
	lkl_sys_halt();
	g_kernel_started = 0;
}

/* ── Disk management ──────────────────────────────────────────── */

int anyfs_disk_add(const char* image_path, uint32_t flags)
{
	if (!image_path || !g_kernel_started)
		return -1;

	/* Find free slot */
	int slot = -1;
	for (int i = 0; i < MAX_DISKS; i++) {
		if (!g_disks[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -1;

	int readonly = (flags & ANYFS_DISK_READONLY) ? 1 : 0;

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

	/* Auto-detect: prefer QEMU if available, else raw */
	if (!ops) {
#ifdef ANYFS_HAS_QEMU
		ops = &qemu_backend_ops;
#else
		ops = &raw_backend_ops;
#endif
	}

	struct lkl_disk disk;
	int ret = ops->open(image_path, readonly, &disk);
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
	for (int i = 0; i < MAX_DISKS; i++) {
		if (g_disks[i].in_use && g_disks[i].disk_id == disk_id) {
			lkl_disk_remove(g_disks[i].disk);
			g_disks[i].backend->close(&g_disks[i].disk);
			g_disks[i].in_use = 0;
			return 0;
		}
	}
	return -1;
}
