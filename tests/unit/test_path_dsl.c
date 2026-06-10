// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the path DSL parser (src/core/anyfs_path.c).
 * Pure userspace — no LKL boot, runs in milliseconds under `meson test`. */
#include "anyfs_path.h"

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

static void test_simple(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("p1", &ap) == 0);
	CHECK(ap.n_comp == 1);
	CHECK(ap.comp[0].p == 1);
	CHECK(ap.comp[0].query == NULL);
	CHECK(ap.disk_idx_set == 0);
	anyfs_path_free(&ap);
}

static void test_disk_prefix(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("disk0/p1", &ap) == 0);
	CHECK(ap.disk_idx_set == 1);
	CHECK(ap.disk_idx == 0);
	CHECK(ap.n_comp == 1 && ap.comp[0].p == 1);
	anyfs_path_free(&ap);

	CHECK(anyfs_path_parse("disk12/p2/p1", &ap) == 0);
	CHECK(ap.disk_idx == 12);
	CHECK(ap.n_comp == 2);
	CHECK(ap.comp[0].p == 2 && ap.comp[1].p == 1);
	anyfs_path_free(&ap);
}

static void test_slashes_and_case(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("/p1/", &ap) == 0); /* leading+trailing ok */
	CHECK(ap.n_comp == 1 && ap.comp[0].p == 1);
	anyfs_path_free(&ap);

	CHECK(anyfs_path_parse("P3", &ap) == 0); /* uppercase P accepted */
	CHECK(ap.comp[0].p == 3);
	anyfs_path_free(&ap);
}

static void test_query(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("p1?keyref=LUKS_KEY", &ap) == 0);
	CHECK(ap.comp[0].query != NULL);
	CHECK(strcmp(ap.comp[0].query, "keyref=LUKS_KEY") == 0);
	anyfs_path_free(&ap);

	/* Query is percent-decoded in place; %2F must not re-split. */
	CHECK(anyfs_path_parse("p1?keyfile=%2Ftmp%2Fk", &ap) == 0);
	CHECK(strcmp(ap.comp[0].query, "keyfile=/tmp/k") == 0);
	anyfs_path_free(&ap);

	/* Bad escape in query is a parse error. */
	CHECK(anyfs_path_parse("p1?key=%zz", &ap) == -1);
}

static void test_errors(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("", &ap) == -1);
	CHECK(anyfs_path_parse("/", &ap) == -1);
	CHECK(anyfs_path_parse("p0", &ap) == -1);   /* index must be > 0 */
	CHECK(anyfs_path_parse("p", &ap) == -1);
	CHECK(anyfs_path_parse("x1", &ap) == -1);
	CHECK(anyfs_path_parse("disk0", &ap) == -1); /* disk alone invalid */
	CHECK(anyfs_path_parse("disk/p1", &ap) == -1);
	CHECK(anyfs_path_parse("diskX/p1", &ap) == -1);
	CHECK(anyfs_path_parse("p1x", &ap) == -1);
	/* 9 components > ANYFS_PATH_MAX_COMPONENTS (8) */
	CHECK(anyfs_path_parse("p1/p1/p1/p1/p1/p1/p1/p1/p1", &ap) == -1);
	CHECK(anyfs_path_parse(NULL, &ap) == -1);
}

static void test_pct_decode(void)
{
	char a[] = "a%2Fb";
	CHECK(anyfs_path_pct_decode(a) == 0);
	CHECK(strcmp(a, "a/b") == 0);

	char b[] = "plain";
	CHECK(anyfs_path_pct_decode(b) == 0);
	CHECK(strcmp(b, "plain") == 0);

	char c[] = "%zz";
	CHECK(anyfs_path_pct_decode(c) == -1);

	char d[] = "%4"; /* truncated escape */
	CHECK(anyfs_path_pct_decode(d) == -1);

	CHECK(anyfs_path_pct_decode(NULL) == 0);
}

int main(void)
{
	test_simple();
	test_disk_prefix();
	test_slashes_and_case();
	test_query();
	test_errors();
	test_pct_decode();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_path_dsl: all OK\n");
	return 0;
}
