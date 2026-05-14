/*
 * share_spec.c — Implementation of the `--share` parsing helpers.
 */
#define _GNU_SOURCE
#include "share_spec.h"

#include <stdio.h>
#include <string.h>

void anyfs_share_auto_name(const char* path, char* out, size_t out_sz)
{
	if (out_sz == 0)
		return;
	size_t i, len = strlen(path);
	if (len >= out_sz)
		len = out_sz - 1;
	for (i = 0; i < len; i++)
		out[i] = (path[i] == '/') ? '_' : path[i];
	out[len] = '\0';
}

int anyfs_share_split(char* spec, const char** name_out, const char** path_out)
{
	char* eq = strchr(spec, '=');
	if (eq) {
		*eq = '\0';
		*name_out = spec;
		*path_out = eq + 1;
	} else {
		*name_out = NULL;
		*path_out = spec;
	}
	return 0;
}

void anyfs_share_warn_literal_key(const AnyfsPath* ap, const char* share_name)
{
	for (size_t i = 0; i < ap->n_comp; i++) {
		const char* q = ap->comp[i].query;
		if (!q || *q == '\0')
			continue;
		const char* kv = q;
		while (kv) {
			if (strncmp(kv, "key=", 4) == 0) {
				fprintf(stderr,
					"warning: --share %s uses literal "
					"'key=...' — "
					"the secret is visible in 'ps'. "
					"Use 'keyref=<envvar>' instead.\n",
					share_name);
				return;
			}
			kv = strchr(kv, '&');
			if (kv)
				kv++;
		}
	}
}
