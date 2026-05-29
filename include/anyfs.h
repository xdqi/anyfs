/*
 * anyfs.h — Umbrella header for anyfs-reader.
 *
 * Include this single header to get the full public API:
 * kernel lifecycle, session management, share resolution,
 * path DSL parsing, table formatting, and the string buffer.
 */
#ifndef ANYFS_H
#define ANYFS_H

#include <lkl.h>
#include <lkl_host.h>
#include <stdint.h>

/* ── Sub-headers in dependency order ────────────────── */
#include "anyfs_format.h"
#include "anyfs_kernel.h"
#include "anyfs_path.h"
#include "anyfs_session.h"
#include "anyfs_share.h"
#include "anyfs_strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Shared flags ──────────────────────────────────────── */

/* Flags for anyfs_session_open:
 *   ANYFS_SESSION_READONLY  — open disk read-only
 *   ANYFS_BACKEND_RAW    — force raw (pread/pwrite) backend
 *   ANYFS_BACKEND_GIO    — force GIO backend
 *   ANYFS_BACKEND_QEMU   — force QEMU block backend
 *   ANYFS_SESSION_SNAPSHOT — open disk with temporary write overlay
 *                            (QEMU backend: BDRV_O_SNAPSHOT)
 *   ANYFS_MOUNT_RDONLY   — mount filesystem read-only (used by
 *                           anyfs_session_enter / anyfs_session_enter_path)
 */
#define ANYFS_SESSION_READONLY (1u << 0)
#define ANYFS_BACKEND_RAW (1u << 1)
#define ANYFS_BACKEND_GIO (1u << 2)
#define ANYFS_BACKEND_QEMU (1u << 3)
#define ANYFS_SESSION_SNAPSHOT (1u << 4)
#define ANYFS_MOUNT_RDONLY (1u << 0)

/* ── Internal types ────────────────────────────────────── */

typedef struct {
	char mount_point[64]; /* Filled: actual mount path (/lklmnt/<name>) */
	char fstype[32];      /* Filled: detected filesystem type */
} AnyfsMount;

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_H */
