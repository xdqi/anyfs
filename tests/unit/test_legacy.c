// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the CLIs' legacy file-name encoding (src/core/anyfs_legacy.c).
 */
#include "anyfs_legacy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANYFS_MOUNT_FAT_CP_SHIFT 8

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,          \
				__LINE__, #cond);                              \
			failures++;                                            \
		}                                                              \
	} while (0)

static void expect_decode(int enc, const char* in, const char* want)
{
	char out[128];
	anyfs_legacy_set(enc);
	int n = anyfs_legacy_decode(in, out, sizeof(out));
	if (n < 0 || strcmp(out, want) != 0) {
		fprintf(stderr,
			"FAIL decode (enc %d): rc=%d got \"%s\", want \"%s\"\n",
			enc, n, out, want);
		failures++;
	}
}

#ifndef _WIN32
static int auto_for(const char* lang)
{
	unsetenv("LC_ALL");
	unsetenv("LC_CTYPE");
	setenv("LANG", lang, 1);
	return anyfs_legacy_auto();
}
#endif

int main(void)
{
	CHECK(anyfs_legacy_parse("gb18030") == ANYFS_LEGACY_GB18030);
	CHECK(anyfs_legacy_parse("big5") == ANYFS_LEGACY_BIG5);
	CHECK(anyfs_legacy_parse("shift_jis") == ANYFS_LEGACY_SHIFT_JIS);
	CHECK(anyfs_legacy_parse("euc-kr") == ANYFS_LEGACY_EUC_KR);
	CHECK(anyfs_legacy_parse("windows-1252") == ANYFS_LEGACY_WINDOWS_1252);
	CHECK(anyfs_legacy_parse("off") == ANYFS_LEGACY_OFF);
	CHECK(anyfs_legacy_parse("klingon") == -1);
	CHECK(anyfs_legacy_parse(NULL) == -1);

#ifndef _WIN32
	CHECK(auto_for("zh_CN.UTF-8") == ANYFS_LEGACY_GB18030);
	CHECK(auto_for("zh_TW.UTF-8") == ANYFS_LEGACY_BIG5);
	CHECK(auto_for("zh_HK") == ANYFS_LEGACY_BIG5);
	CHECK(auto_for("ja_JP.UTF-8") == ANYFS_LEGACY_SHIFT_JIS);
	CHECK(auto_for("ko_KR.EUC-KR") == ANYFS_LEGACY_EUC_KR);
	CHECK(auto_for("en_US.UTF-8") == ANYFS_LEGACY_WINDOWS_1252);
	CHECK(auto_for("C") == ANYFS_LEGACY_WINDOWS_1252);
	setenv("LC_ALL", "zh_CN.GB18030", 1); /* LC_ALL wins over LANG */
	CHECK(anyfs_legacy_auto() == ANYFS_LEGACY_GB18030);
	CHECK(anyfs_legacy_parse("auto") == ANYFS_LEGACY_GB18030);
	unsetenv("LC_ALL");
#endif

	CHECK(anyfs_legacy_fat_flag(ANYFS_LEGACY_GB18030) ==
	      (1u << ANYFS_MOUNT_FAT_CP_SHIFT));
	CHECK(anyfs_legacy_fat_flag(ANYFS_LEGACY_BIG5) ==
	      (2u << ANYFS_MOUNT_FAT_CP_SHIFT));
	CHECK(anyfs_legacy_fat_flag(ANYFS_LEGACY_SHIFT_JIS) ==
	      (3u << ANYFS_MOUNT_FAT_CP_SHIFT));
	CHECK(anyfs_legacy_fat_flag(ANYFS_LEGACY_EUC_KR) ==
	      (4u << ANYFS_MOUNT_FAT_CP_SHIFT));
	CHECK(anyfs_legacy_fat_flag(ANYFS_LEGACY_WINDOWS_1252) == 0);
	CHECK(anyfs_legacy_fat_flag(ANYFS_LEGACY_OFF) == 0);

	/* UTF-8 is shown as it is, whatever the encoding. */
	expect_decode(ANYFS_LEGACY_GB18030, "\xe4\xb8\xad\xe6\x96\x87",
		      "\xe4\xb8\xad\xe6\x96\x87");
	expect_decode(ANYFS_LEGACY_OFF, "plain", "plain");
	/* Legacy bytes decode with the encoding. */
	expect_decode(ANYFS_LEGACY_GB18030, "\xd6\xd0\xce\xc4",
		      "\xe4\xb8\xad\xe6\x96\x87"); /* 中文 */
	expect_decode(ANYFS_LEGACY_WINDOWS_1252, "caf\xe9",
		      "caf\xc3\xa9"); /* café */
	expect_decode(ANYFS_LEGACY_BIG5, "\xa4\xa4\xa4\xe5",
		      "\xe4\xb8\xad\xe6\x96\x87");
	expect_decode(ANYFS_LEGACY_SHIFT_JIS, "\x83\x65\x83\x58\x83\x67",
		      "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"); /* テスト */
	/* Off, or bytes the encoding cannot decode: \xNN. */
	expect_decode(ANYFS_LEGACY_OFF, "\xd6\xd0.txt", "\\xD6\\xD0.txt");
	expect_decode(ANYFS_LEGACY_SHIFT_JIS, "a\xff", "a\\xFF");
	{
		char small[4];
		anyfs_legacy_set(ANYFS_LEGACY_OFF);
		CHECK(anyfs_legacy_decode("\xd6", small, sizeof(small)) == -1);
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("legacy: all passed\n");
	return 0;
}
