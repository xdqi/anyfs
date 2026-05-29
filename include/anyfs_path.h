/*
 * path_dsl.h — Parse `[disk<N>/]p<n>[?<query>](/p<m>...)*`
 *
 * Shared by every surface so they all accept the same canonical
 * partition reference.
 */
#ifndef ANYFS_PATH_H
#define ANYFS_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANYFS_PATH_MAX_COMPONENTS 8

typedef struct AnyfsPathComp {
	unsigned int p;	   /* partition index (>0) */
	const char* query; /* NULL or arena-owned, already URL-decoded */
} AnyfsPathComp;

typedef struct {
	/* Arena memory backing everything below. Free with
	 * anyfs_path_free. */
	char* arena;

	int disk_idx_set; /* 1 if `diskN/` was explicit */
	int disk_idx;	  /* 0 if not set */

	size_t n_comp;
	AnyfsPathComp comp[ANYFS_PATH_MAX_COMPONENTS];
} AnyfsPath;

/* Parse `s` into `out`. Returns 0 on success, negative on parse error.
 * `out->arena` must be released with anyfs_path_free. On error the
 * struct is in a safe zeroed state. */
int anyfs_path_parse(const char* s, AnyfsPath* out);
void anyfs_path_free(AnyfsPath* p);

/* Percent-decode `s` in place. Returns 0 on success; -1 on a bad
 * escape (the buffer is left as-is). */
int anyfs_path_pct_decode(char* s);

#ifdef __cplusplus
}
#endif

#endif
