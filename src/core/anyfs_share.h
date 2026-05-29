/*
 * share_spec.h — Shared `--share name=path` helpers for server surfaces.
 *
 * Used by lkl_ksmbd and lkl_nfsd. Both binaries parse the same
 * `name=path[?query]` form and reject literal `key=` credentials with
 * the same warning, so the logic lives here.
 */
#ifndef ANYFS_ANYFS_SHARE_H
#define ANYFS_ANYFS_SHARE_H

#include "anyfs_path.h"
#include "anyfs_session.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slash-to-underscore mapping for auto-derived share names from a path.
 * `"disk0/p1"` → `"disk0_p1"`. */
void anyfs_share_auto_name(const char* path, char* out, size_t out_sz);

/* Split `name=path` in place (modifies `spec` by NUL-terminating at
 * the `=`). If there's no `=`, `*name_out` is set to NULL and the whole
 * string is `*path_out` — caller is expected to derive the name. */
int anyfs_share_split(char* spec, const char** name_out, const char** path_out);

/* Walk an AnyfsPath's query strings and emit a stderr warning if any
 * component uses literal `key=...` (secret visible to `ps`). Other
 * credential forms (`keyref=`, `keyfile=`, `keyfd=`) are accepted
 * silently. `share_name` is used purely for the warning message. */
void anyfs_share_warn_literal_key(const AnyfsPath* ap, const char* share_name);

/* Resolve a --share spec string ("name=disk0/p1", "disk0/p1", bare "p1",
 * bare "1") to a share name and an LKL mount path. Abstracts the
 * bare-int→p<N> back-compat, disk0/ auto-prefix in single-disk mode,
 * path-DSL parsing, disk-index validation, and anyfs_disk_enter_path
 * sequence that was duplicated across ksmbd and nfsd.
 *
 * On success returns 0 and fills name_out + lkl_out. On failure prints a
 * diagnostic to stderr and returns -1. */
int anyfs_share_resolve(const char* spec, AnyfsDisk** disks, int n_disks,
			uint32_t enter_flags, char* name_out, size_t name_sz,
			char* lkl_out, size_t lkl_sz);

/* Open all disk images in a loop. Abstracts the ?<query> suffix stripping
 * and error reporting duplicated across FUSE, ksmbd, and nfsd. On failure
 * prints a diagnostic and closes any disks already opened.
 *
 * disks_out must be pre-allocated with at least n_images slots. Returns 0
 * on success, -1 on failure. */
int anyfs_share_open_disks(AnyfsDisk** disks_out, const char** images,
			   int n_images, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
