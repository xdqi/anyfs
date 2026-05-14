/*
 * share_spec.h — Shared `--share name=path` helpers for server surfaces.
 *
 * Used by lkl_ksmbd and lkl_nfsd. Both binaries parse the same
 * `name=path[?query]` form and reject literal `key=` credentials with
 * the same warning, so the logic lives here.
 */
#ifndef ANYFS_SHARE_SPEC_H
#define ANYFS_SHARE_SPEC_H

#include "path_dsl.h"
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

#ifdef __cplusplus
}
#endif

#endif
