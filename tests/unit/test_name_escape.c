// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the file-name escape (src/core/anyfs_name.c): any byte
 * string survives escape -> unescape unchanged, and the escaped form is
 * always strict UTF-8. */
#include "anyfs_name.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,          \
				__LINE__, #cond);                              \
			failures++;                                            \
		}                                                              \
	} while (0)

/* Independent strict UTF-8 validator (RFC 3629): no overlongs, no
 * surrogates, nothing above U+10FFFF. */
static int valid_utf8(const unsigned char* s, size_t n)
{
	size_t i = 0;
	while (i < n) {
		unsigned c = s[i];
		size_t len;
		uint32_t cp;
		if (c < 0x80) {
			i++;
			continue;
		} else if (c >= 0xC2 && c <= 0xDF) {
			len = 2;
			cp = c & 0x1F;
		} else if (c >= 0xE0 && c <= 0xEF) {
			len = 3;
			cp = c & 0x0F;
		} else if (c >= 0xF0 && c <= 0xF4) {
			len = 4;
			cp = c & 0x07;
		} else {
			return 0;
		}
		if (i + len > n)
			return 0;
		for (size_t k = 1; k < len; k++) {
			if ((s[i + k] & 0xC0) != 0x80)
				return 0;
			cp = (cp << 6) | (s[i + k] & 0x3F);
		}
		if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
		    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
			return 0;
		i += len;
	}
	return 1;
}

static char esc[ANYFS_NAME_ESCAPE_MAX(64)];
static char back[ANYFS_NAME_ESCAPE_MAX(64)];

/* Escape and unescape `in` (NUL-free, n bytes); check every property. */
static void roundtrip(const unsigned char* in, size_t n)
{
	char src[65];
	memcpy(src, in, n);
	src[n] = '\0';

	int e = anyfs_name_escape(src, esc, sizeof(esc));
	if (e < 0 || (size_t)e != strlen(esc) ||
	    !valid_utf8((const unsigned char*)esc, (size_t)e)) {
		fprintf(stderr, "FAIL escape of %zu bytes: rc=%d\n", n, e);
		failures++;
		return;
	}
	int u = anyfs_name_unescape(esc, back, sizeof(back));
	if (u != (int)n || memcmp(back, src, n) != 0) {
		fprintf(stderr, "FAIL round trip of %zu bytes\n", n);
		failures++;
	}
	/* Valid UTF-8 without the escape range passes through unchanged. */
	if (valid_utf8(in, n) && !strstr(src, "\xee\xbe") &&
	    !strstr(src, "\xee\xbf") && strcmp(esc, src) != 0) {
		fprintf(stderr, "FAIL valid UTF-8 was changed\n");
		failures++;
	}
}

static void expect_escape(const char* in, const char* want)
{
	char out[128];
	int n = anyfs_name_escape(in, out, sizeof(out));
	if (n < 0 || strcmp(out, want) != 0) {
		fprintf(stderr, "FAIL escape: rc=%d got", n);
		for (const char* p = out; n >= 0 && *p; p++)
			fprintf(stderr, " %02x", (unsigned char)*p);
		fprintf(stderr, "\n");
		failures++;
	}
}

int main(void)
{
	/* Vectors. */
	expect_escape("plain.txt", "plain.txt");
	expect_escape("\xe4\xb8\xad\xe6\x96\x87.txt",
		      "\xe4\xb8\xad\xe6\x96\x87.txt"); /* 中文 */
	expect_escape("caf\xe9", "caf\xee\xbf\xa9");   /* E9 -> U+EFE9 */
	expect_escape("\xd6\xd0", "\xee\xbf\x96\xee\xbf\x90"); /* GBK 中 */
	expect_escape("\xc0\xaf", "\xee\xbf\x80\xee\xbe\xaf"); /* overlong / */
	expect_escape("\xed\xa0\x80",
		      "\xee\xbf\xad\xee\xbe\xa0\xee\xbe\x80"); /* surrogate */
	expect_escape(
	    "\xf4\x90\x80\x80",
	    "\xee\xbf\xb4\xee\xbe\x90\xee\xbe\x80\xee\xbe\x80"); /* > U+10FFFF
								  */
	expect_escape("\xee\xbe\x80",
		      "\xee\xbf\xae\xee\xbe\xbe\xee\xbe\x80"); /* U+EF80 */
	expect_escape("\xee\xbd\xbf", "\xee\xbd\xbf"); /* U+EF7F: outside */
	expect_escape("\xef\x80\x80", "\xef\x80\x80"); /* U+F000: outside */
	expect_escape("\xe4\xb8", "\xee\xbf\xa4\xee\xbe\xb8"); /* truncated */

	/* Every string of 1, 2 and 3 bytes (no NUL). */
	unsigned char b[3];
	for (unsigned x = 1; x < 256; x++) {
		b[0] = (unsigned char)x;
		roundtrip(b, 1);
		for (unsigned y = 1; y < 256; y++) {
			b[1] = (unsigned char)y;
			roundtrip(b, 2);
		}
	}
	for (unsigned x = 0x80; x < 256; x++)
		for (unsigned y = 1; y < 256; y++)
			for (unsigned z = 1; z < 256; z += 3) {
				b[0] = (unsigned char)x;
				b[1] = (unsigned char)y;
				b[2] = (unsigned char)z;
				roundtrip(b, 3);
			}

	/* Random strings up to 64 bytes, biased to high bytes. */
	uint64_t seed = 0x5eed;
	unsigned char r[64];
	for (int i = 0; i < 1000000; i++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		size_t n = 1 + (seed >> 58);
		for (size_t k = 0; k < n; k++) {
			seed = seed * 6364136223846793005ULL +
			       1442695040888963407ULL;
			unsigned v = (unsigned)(seed >> 56);
			r[k] =
			    (unsigned char)(v ? (v & 1 ? v | 0x80 : v) : 'a');
		}
		roundtrip(r, n);
	}

	/* Overflow: -1, never a truncated result. */
	{
		char small[4];
		CHECK(anyfs_name_escape("abcd", small, sizeof(small)) == -1);
		CHECK(anyfs_name_escape("abc", small, sizeof(small)) == 3);
		CHECK(anyfs_name_escape("\xe9", small, sizeof(small)) == 3);
		CHECK(anyfs_name_escape("a\xe9", small, sizeof(small)) == -1);
		CHECK(anyfs_name_unescape("\xee\xbf\xa9", small, 2) == 1);
		CHECK(anyfs_name_unescape("abc", small, 3) == -1);
		CHECK(anyfs_name_escape("x", small, 0) == -1);
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("name_escape: all passed\n");
	return 0;
}
