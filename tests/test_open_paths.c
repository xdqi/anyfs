/*
 * test_open_paths.c — open images by host path through the core, with the
 * backend and mode flags given on the command line. Run under wine by
 * tests/wine/u8-cli.sh with images in non-ASCII directories and %TEMP%
 * pointing at another one: covers the raw backend's CreateFileW, QEMU's
 * patched file-win32 open, the snapshot overlay in %TEMP%, and the probe
 * spool, the same core code the Electron addon links.
 *
 * Usage: test_open_paths <open-flags> <image> [<open-flags> <image> ...]
 * Each image must be one from tests/make_names_image.py: the first vfat
 * partition is entered read-only and 中文.txt must read "fat-cn\n".
 *
 * The enter runs on a fresh thread, as the Electron addon's does on a libuv
 * worker: the mount then happens a few KiB below the top of the stack.
 * Regression: mount(2) copies a whole page from its data argument, and
 * anyfs passed a 64-byte stack buffer; on Windows the read ran past the top
 * of the thread's stack and the process crashed.
 */
#include "anyfs.h"

#include <lkl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define STEP(...)                                                              \
	do {                                                                   \
		if (getenv("ANYFS_TEST_STEPS")) {                              \
			fprintf(stderr, __VA_ARGS__);                          \
			fputc('\n', stderr);                                   \
		}                                                              \
	} while (0)

struct enter_args {
	AnyfsSession* s;
	unsigned part;
	char* mnt;
	int rc;
};

static DWORD WINAPI enter_thread(LPVOID p)
{
	struct enter_args* a = p;
	a->rc = anyfs_session_enter(a->s, a->part, ANYFS_MOUNT_RDONLY, a->mnt);
	return 0;
}

static int enter_on_thread(AnyfsSession* s, unsigned part, char* mnt)
{
	struct enter_args a = {s, part, mnt, -1};
	HANDLE h = CreateThread(NULL, 256 * 1024, enter_thread, &a,
				STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	if (!h)
		return -1;
	WaitForSingleObject(h, INFINITE);
	CloseHandle(h);
	return a.rc;
}

static int check_image(const char* img, uint32_t flags)
{
	AnyfsSession* s = NULL;
	STEP("open %s", img);
	if (anyfs_session_open(img, flags, &s) != 0 || !s) {
		printf("FAIL %s (flags 0x%x): open\n", img, flags);
		return 1;
	}
	AnyfsPartInfo parts[16];
	size_t got = 0;
	int n = anyfs_session_list(s, -1, parts, 16, &got);
	int idx = -1;
	for (int i = 0; i < n && i < 16; i++)
		if (strcmp(parts[i].fstype, "vfat") == 0)
			idx = (int)parts[i].index;
	char mnt[ANYFS_LKL_PATH_MAX];
	STEP("enter p%d", idx);
	if (idx < 0 || enter_on_thread(s, (unsigned)idx, mnt) != 0) {
		printf("FAIL %s (flags 0x%x): no vfat partition to enter\n",
		       img, flags);
		anyfs_session_close(s);
		return 1;
	}

	char path[ANYFS_LKL_PATH_MAX + 32], buf[64] = {0};
	snprintf(path, sizeof(path), "%s/中文.txt", mnt);
	STEP("read %s", path);
	long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
	long r = fd >= 0 ? lkl_sys_read(fd, buf, sizeof(buf) - 1) : fd;
	if (fd >= 0)
		lkl_sys_close(fd);
	STEP("close");
	anyfs_session_close(s);
	if (r < 0 || strcmp(buf, "fat-cn\n") != 0) {
		printf("FAIL %s (flags 0x%x): read %s -> %ld\n", img, flags,
		       path, r);
		return 1;
	}
	printf("ok   %s (flags 0x%x)\n", img, flags);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 3 || argc % 2 == 0) {
		fprintf(stderr, "usage: %s <open-flags> <image> ...\n",
			argv[0]);
		return 2;
	}
	AnyfsKernelOpts opts = {0};
	if (anyfs_kernel_init(&opts) != 0) {
		fprintf(stderr, "kernel init failed\n");
		return 1;
	}
	int fail = 0;
	for (int i = 1; i + 1 < argc; i += 2)
		fail |= check_image(argv[i + 1],
				    (uint32_t)strtoul(argv[i], NULL, 0));
	STEP("halt");
	anyfs_kernel_halt();
	STEP("done");
	return fail;
}
