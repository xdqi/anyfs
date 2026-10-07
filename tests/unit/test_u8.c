// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the UTF-8 host layer (src/win32/anyfs_u8.c). The prefix
 * scan runs everywhere; the W-API half runs on Windows (under wine via
 * meson's exe_wrapper when cross-built). */
#include "anyfs_u8.h"
#include "anyfs_u8_win.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,          \
				__LINE__, #cond);                              \
			failures++;                                            \
		}                                                              \
	} while (0)

static size_t prefix(const char* s)
{
	return anyfs_u8_complete_prefix(s, strlen(s));
}

#ifdef _WIN32
#define NAME                                                                    \
	"\xe6\xb5\x8b\xe8\xaf\x95 caf\xc3\xa9 \xf0\x9f\x98\x80" /* 测试 café \
								   😀 */

static void test_conversions(void)
{
	wchar_t* w = NULL;
	char* back = NULL;

	CHECK(anyfs_u8_to_u16(NAME, &w) == 0);
	CHECK(w && wcscmp(w, L"测试 café \U0001F600") == 0);
	CHECK(anyfs_u16_to_u8(w, &back) == 0 && strcmp(back, NAME) == 0);
	free(w);
	free(back);
	/* Strict: an encoded surrogate and a lone byte are errors. */
	CHECK(anyfs_u8_to_u16("\xed\xa0\x80", &w) == -1);
	CHECK(anyfs_u8_to_u16("caf\xe9", &w) == -1);
}

static void test_files(void)
{
	char dir[MAX_PATH * 3], path[MAX_PATH * 4];
	wchar_t wtmp[MAX_PATH + 1];
	char* tmp = NULL;

	CHECK(GetTempPathW(MAX_PATH + 1, wtmp) > 0);
	CHECK(anyfs_u16_to_u8(wtmp, &tmp) == 0);
	snprintf(dir, sizeof(dir), "%s" NAME, tmp);
	free(tmp);
	wchar_t* wdir = NULL;
	CHECK(anyfs_u8_to_u16(dir, &wdir) == 0);
	CHECK(CreateDirectoryW(wdir, NULL) ||
	      GetLastError() == ERROR_ALREADY_EXISTS);

	snprintf(path, sizeof(path), "%s\\" NAME ".txt", dir);
	int fd =
	    anyfs_u8_open(path, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY, 0644);
	CHECK(fd >= 0 && _write(fd, "hi", 2) == 2);
	if (fd >= 0)
		_close(fd);

	struct stat st;
	CHECK(anyfs_u8_stat(path, &st) == 0 && st.st_size == 2);
	CHECK(anyfs_u8_access(path, 0) == 0);
	FILE* f = anyfs_u8_fopen(path, "rb");
	char buf[4] = {0};
	CHECK(f && fread(buf, 1, 2, f) == 2 && memcmp(buf, "hi", 2) == 0);
	if (f)
		fclose(f);
	HANDLE h = anyfs_u8_create_file(path, GENERIC_READ, FILE_SHARE_READ,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
	CHECK(h != INVALID_HANDLE_VALUE);
	CloseHandle(h);
	CHECK(anyfs_u8_unlink(path) == 0);
	CHECK(anyfs_u8_access(path, 0) == -1);

	snprintf(path, sizeof(path), "%s\\t-XXXXXX", dir);
	fd = anyfs_u8_mkstemp(path);
	CHECK(fd >= 0 && strncmp(path, dir, strlen(dir)) == 0 &&
	      strcmp(path + strlen(path) - 6, "XXXXXX") != 0);
	if (fd >= 0)
		_close(fd);
	CHECK(anyfs_u8_unlink(path) == 0);

	/* A temp file that vanishes on close. */
	fd = anyfs_u8_tmpfile_fd();
	CHECK(fd >= 0 && _write(fd, "x", 1) == 1);
	if (fd >= 0)
		_close(fd);

	CHECK(RemoveDirectoryW(wdir));
	free(wdir);
}

static void test_getenv(void)
{
	CHECK(SetEnvironmentVariableW(L"ANYFS_U8_TEST", L"测试"));
	const char* v = anyfs_u8_getenv("ANYFS_U8_TEST");
	CHECK(v && strcmp(v, "\xe6\xb5\x8b\xe8\xaf\x95") == 0);
	CHECK(anyfs_u8_getenv("ANYFS_U8_TEST") == v); /* stable pointer */
	CHECK(anyfs_u8_getenv("ANYFS_U8_UNSET_VAR") == NULL);
}
#endif

int main(void)
{
	CHECK(prefix("") == 0);
	CHECK(prefix("a") == 1);
	CHECK(prefix("\xe4\xb8") == 0);	    /* 中, 2 of 3 bytes */
	CHECK(prefix("\xe4\xb8\xad") == 3); /* 中 */
	CHECK(prefix("a\xe4") == 1);
	CHECK(prefix("x\xf0\x9f") == 1); /* 😀, 2 of 4 bytes */
	CHECK(prefix("x\xf0\x9f\x98\x80") == 5);
	CHECK(prefix("\xff") == 1);	/* not a lead byte: passes through */
	CHECK(prefix("\x80\x80") == 2); /* stray continuations pass through */
	CHECK(prefix("ab\xc3") == 2);

#ifdef _WIN32
	test_conversions();
	test_files();
	test_getenv();
#endif

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("u8: all passed\n");
	return 0;
}
