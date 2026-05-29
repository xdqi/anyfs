/*
 * path_dsl.c — Parser for [disk<N>/]p<n>[?<query>](/p<m>...)*
 */
#define _GNU_SOURCE
#include "anyfs_path.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nyb(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int anyfs_path_dsl_pct_decode(char* s)
{
	if (!s)
		return 0;
	char* w = s;
	for (char* r = s; *r;) {
		if (*r == '%') {
			if (!r[1] || !r[2])
				return -1;
			int hi = hex_nyb((unsigned char)r[1]);
			int lo = hex_nyb((unsigned char)r[2]);
			if (hi < 0 || lo < 0)
				return -1;
			*w++ = (char)((hi << 4) | lo);
			r += 3;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
	return 0;
}

static int parse_disk_prefix(char* seg, int* out_idx)
{
	/* "disk" + digits, nothing else. Return 1 on match, 0 if no
	 * match, -1 on syntactic error. */
	if (strncmp(seg, "disk", 4) != 0)
		return 0;
	const char* r = seg + 4;
	if (!*r)
		return -1;
	if (!isdigit((unsigned char)*r))
		return -1;
	char* end = NULL;
	long v = strtol(r, &end, 10);
	if (!end || *end != '\0' || v < 0)
		return -1;
	*out_idx = (int)v;
	return 1;
}

static int parse_component(char* seg, AnyfsPathComp* out)
{
	/* "p" + digits, optional "?<query>". */
	if (seg[0] != 'p' && seg[0] != 'P')
		return -1;
	char* q = strchr(seg, '?');
	if (q) {
		*q = '\0';
		out->query = q + 1;
	} else {
		out->query = NULL;
	}
	const char* r = seg + 1;
	if (!*r)
		return -1;
	char* end = NULL;
	unsigned long v = strtoul(r, &end, 10);
	if (!end || *end != '\0' || v == 0)
		return -1;
	out->p = (unsigned int)v;
	if (out->query) {
		/* URL-decode the query string in place; encoded slashes were
		 * preserved because splitting happened first. */
		if (anyfs_path_dsl_pct_decode((char*)out->query) < 0)
			return -1;
	}
	return 0;
}

void anyfs_path_dsl_free(AnyfsPath* p)
{
	if (!p)
		return;
	free(p->arena);
	memset(p, 0, sizeof(*p));
}

int anyfs_path_dsl_parse(const char* s, AnyfsPath* out)
{
	if (!s || !out)
		return -1;
	memset(out, 0, sizeof(*out));

	/* Skip leading slash. Strip trailing slash. */
	while (*s == '/')
		s++;
	char* arena = strdup(s);
	if (!arena)
		return -1;
	out->arena = arena;
	size_t L = strlen(arena);
	while (L > 0 && arena[L - 1] == '/')
		arena[--L] = '\0';

	if (L == 0) {
		/* Empty path is a parse error here; surfaces that allow "the
		 * disk root" handle that themselves. */
		anyfs_path_dsl_free(out);
		return -1;
	}

	/* Split on '/' (but only outside ?<query> values — encoded '/'
	 * inside `keyfile=` must NOT split. The encoded form is %2F, so
	 * the raw input has no literal '/' inside a value, and we can
	 * just split on every '/'. */
	char* segs[ANYFS_PATH_DSL_MAX_COMPONENTS + 1];
	size_t nseg = 0;
	char* p = arena;
	while (p && *p) {
		if (nseg >= sizeof(segs) / sizeof(segs[0])) {
			anyfs_path_dsl_free(out);
			return -1;
		}
		segs[nseg++] = p;
		char* slash = strchr(p, '/');
		if (slash) {
			*slash = '\0';
			p = slash + 1;
		} else {
			p = NULL;
		}
	}
	if (nseg == 0) {
		anyfs_path_dsl_free(out);
		return -1;
	}

	size_t comp_start = 0;
	int disk_idx = 0;
	int dr = parse_disk_prefix(segs[0], &disk_idx);
	if (dr < 0) {
		anyfs_path_dsl_free(out);
		return -1;
	}
	if (dr == 1) {
		out->disk_idx_set = 1;
		out->disk_idx = disk_idx;
		comp_start = 1;
		if (nseg == 1) {
			/* "disk0" alone is not a valid partition path. */
			anyfs_path_dsl_free(out);
			return -1;
		}
	}

	size_t ncomp = nseg - comp_start;
	if (ncomp == 0 || ncomp > ANYFS_PATH_DSL_MAX_COMPONENTS) {
		anyfs_path_dsl_free(out);
		return -1;
	}
	for (size_t i = 0; i < ncomp; i++) {
		if (parse_component(segs[comp_start + i], &out->comp[i]) < 0) {
			anyfs_path_dsl_free(out);
			return -1;
		}
	}
	out->n_comp = ncomp;
	return 0;
}
