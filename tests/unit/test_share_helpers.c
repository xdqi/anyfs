// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the pure --share helpers (src/core/anyfs_share.c). */
#include "anyfs_share.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

static void test_auto_name(void)
{
	char out[32];
	anyfs_share_auto_name("disk0/p1", out, sizeof(out));
	CHECK(strcmp(out, "disk0_p1") == 0);

	anyfs_share_auto_name("p2", out, sizeof(out));
	CHECK(strcmp(out, "p2") == 0);

	/* Truncation: output is always NUL-terminated. */
	char tiny[5];
	anyfs_share_auto_name("disk0/p1", tiny, sizeof(tiny));
	CHECK(strcmp(tiny, "disk") == 0);

	/* out_sz == 0 must not write anything (no crash). */
	anyfs_share_auto_name("x", tiny, 0);
}

static void test_split(void)
{
	const char *name, *path;

	char a[] = "data=disk0/p1";
	CHECK(anyfs_share_split(a, &name, &path) == 0);
	CHECK(name && strcmp(name, "data") == 0);
	CHECK(strcmp(path, "disk0/p1") == 0);

	char b[] = "disk0/p1";
	CHECK(anyfs_share_split(b, &name, &path) == 0);
	CHECK(name == NULL);
	CHECK(strcmp(path, "disk0/p1") == 0);

	/* Only the FIRST '=' splits; the rest stays in path. */
	char c[] = "n=p1?key=v";
	CHECK(anyfs_share_split(c, &name, &path) == 0);
	CHECK(strcmp(name, "n") == 0);
	CHECK(strcmp(path, "p1?key=v") == 0);
}

int main(void)
{
	test_auto_name();
	test_split();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_share_helpers: all OK\n");
	return 0;
}
