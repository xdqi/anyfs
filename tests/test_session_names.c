/*
 * test_session_names.c — file names in every encoding must list, stat and
 * open through the TypeScript glue (ts/native/anyfs_ts.c), the layer every
 * UI backend (browser wasm, Node wasm, native addon) goes through.
 *
 * Regressions covered:
 *   - FAT long names came out through iocharset=iso8859-1: CJK became '?'
 *     and Latin-1 a lone byte that is not UTF-8;
 *   - FAT short (8.3) names are bytes in an OEM codepage; the caller picks
 *     it with ANYFS_MOUNT_FAT_CP_*.
 *
 * The image comes from tests/make_names_image.py (path in argv[1]).
 * Raw backend, read-only. Exit 77 (skip) when the generator can't run.
 */
#include "anyfs.h"

#include <lkl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int anyfs_ts_kernel_init(uint32_t mem_mb, uint32_t loglevel);
int anyfs_ts_session_open(const char* image_path, uint32_t flags);
int anyfs_ts_session_enter(int h, unsigned int part, uint32_t flags,
			   char* mount_out, size_t mount_cap);
int anyfs_ts_readdir_json(const char* path, char* buf, size_t cap);
int anyfs_ts_lstat_json(const char* path, char* buf, size_t cap);
int anyfs_ts_open(const char* path, int flags);
int64_t anyfs_ts_pread(int fd, void* buf, uint32_t n, int64_t off);
int anyfs_ts_close(int fd);

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			exit(1);                                               \
		}                                                              \
	} while (0)

struct entry {
	const char* name;    /* as the glue must report it (UTF-8) */
	const char* content; /* file content */
};

#define MAX_NAMES 16

/* Pull every "name":"..." string out of a readdir JSON array. jsonw.h
 * escapes only '"', '\\', \n, \r, \t and other controls (as \u00XX). */
static int json_names(const char* json, char names[][256], int max)
{
	int n = 0;
	const char* key = "\"name\":\"";
	for (const char* p = strstr(json, key); p && n < max;
	     p = strstr(p, key)) {
		p += strlen(key);
		size_t len = 0;
		while (*p && *p != '"' && len < 255) {
			if (*p == '\\') {
				p++;
				if (*p == 'u') {
					unsigned v = 0;
					sscanf(p + 1, "%4x", &v);
					names[n][len++] = (char)v;
					p += 5;
					continue;
				}
				names[n][len++] = *p == 'n'   ? '\n'
						  : *p == 'r' ? '\r'
						  : *p == 't' ? '\t'
							      : *p;
				p++;
				continue;
			}
			names[n][len++] = *p++;
		}
		names[n][len] = '\0';
		n++;
	}
	return n;
}

static void check_part(int h, unsigned part, uint32_t flags,
		       const struct entry* want, int n_want)
{
	char mnt[256], buf[8192], names[MAX_NAMES][256];

	CHECK(anyfs_ts_session_enter(h, part, flags, mnt, sizeof(mnt)) == 0,
	      "enter p%u", part);
	int rc = anyfs_ts_readdir_json(mnt, buf, sizeof(buf));
	CHECK(rc > 0, "readdir %s -> %d", mnt, rc);
	buf[rc] = '\0';
	printf("p%u flags=0x%x: %s\n", part, flags, buf);

	int n = json_names(buf, names, MAX_NAMES);
	int found = 0;
	for (int i = 0; i < n; i++) {
		if (strcmp(names[i], "lost+found") == 0)
			continue;
		const struct entry* e = NULL;
		for (int j = 0; j < n_want; j++)
			if (strcmp(names[i], want[j].name) == 0)
				e = &want[j];
		CHECK(e, "p%u: unexpected name \"%s\"", part, names[i]);
		found++;

		char path[600], st[1024], data[64];
		snprintf(path, sizeof(path), "%s/%s", mnt, names[i]);
		CHECK(anyfs_ts_lstat_json(path, st, sizeof(st)) > 0,
		      "lstat \"%s\"", path);
		int fd = anyfs_ts_open(path, LKL_O_RDONLY);
		CHECK(fd >= 0, "open \"%s\" -> %d", path, fd);
		int64_t got = anyfs_ts_pread(fd, data, sizeof(data) - 1, 0);
		anyfs_ts_close(fd);
		CHECK(got >= 0, "pread \"%s\"", path);
		data[got] = '\0';
		CHECK(strcmp(data, e->content) == 0,
		      "\"%s\": content \"%s\", want \"%s\"", path, data,
		      e->content);
	}
	CHECK(found == n_want, "p%u: %d of %d names listed", part, found,
	      n_want);
}

int main(int argc, char** argv)
{
	CHECK(argc == 2, "usage: %s <make_names_image.py>", argv[0]);

	char dir[] = "/tmp/anyfs-names-XXXXXX";
	CHECK(mkdtemp(dir), "mkdtemp");
	char img[64], cmd[512];
	snprintf(img, sizeof(img), "%s/names.img", dir);
	snprintf(cmd, sizeof(cmd), "python3 '%s' '%s'", argv[1], img);
	int st = system(cmd);
	if (st != 0) {
		rmdir(dir);
		if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) == 77 ||
		    WEXITSTATUS(st) == 127) {
			printf("skip: cannot generate the image\n");
			return 77;
		}
		CHECK(0, "make_names_image.py failed (%d)", st);
	}

	CHECK(anyfs_ts_kernel_init(64, 0) == 0, "kernel init");
	uint32_t open_flags = ANYFS_SESSION_READONLY | ANYFS_BACKEND_RAW;
	int h = anyfs_ts_session_open(img, open_flags);
	int h936 = anyfs_ts_session_open(img, open_flags);
	CHECK(h >= 0 && h936 >= 0, "open %s", img);

	/* FAT long names are UTF-16 on disk: UTF-8 whatever the codepage.
	 * The 8.3-only name is GBK bytes: box drawing in 437, CJK in 936. */
	const struct entry fat437[] = {
	    {"中文.txt", "fat-cn\n"},
	    {"café.txt", "fat-cafe\n"},
	    {"▓Γ╩╘.TXT", "fat-gbk\n"},
	};
	const struct entry fat936[] = {
	    {"中文.txt", "fat-cn\n"},
	    {"café.txt", "fat-cafe\n"},
	    {"测试.TXT", "fat-gbk\n"},
	};
	check_part(h, 1, 0, fat437, 3);
	check_part(h936, 1, ANYFS_MOUNT_FAT_CP_936, fat936, 3);

	unlink(img);
	rmdir(dir);
	printf("ok\n");
	return 0;
}
