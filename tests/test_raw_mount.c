/*
 * Test: mount a raw disk image via anyfs API, list root directory.
 * Usage: test_raw_mount [--gio] <image> <fstype> <part>
 */
#include "anyfs_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	int use_gio = 0;
	int argoff = 1;

	if (argc > 1 && strcmp(argv[1], "--gio") == 0) {
		use_gio = 1;
		argoff = 2;
	} else if (argc > 1 && strcmp(argv[1], "--raw") == 0) {
		argoff = 2;
	}

	if (argc - argoff < 3) {
		fprintf(stderr,
			"usage: %s [--gio] <image> <fstype> <part>\n"
			"  --gio   use GIO sync backend\n"
			"  part=0 for whole disk, 1+ for partition number\n",
			argv[0]);
		return 1;
	}

	const char* image = argv[argoff];
	const char* fstype = argv[argoff + 1];
	uint32_t part = (uint32_t)atoi(argv[argoff + 2]);

	AnyfsContext* ctx = NULL;
	int32_t ret = anyfs_init(&ctx, NULL);
	if (ret != ANYFS_OK) {
		fprintf(stderr, "anyfs_init failed: %d\n", ret);
		return 1;
	}

	uint32_t flags = ANYFS_OPEN_READONLY;
	if (use_gio) {
#ifdef ANYFS_HAS_GIO
		flags |= ANYFS_OPEN_GIO;
		printf("Using GIO sync backend.\n");
#else
		fprintf(stderr, "GIO backend not compiled in.\n");
		anyfs_destroy(ctx);
		return 1;
#endif
	} else {
		printf("Using raw (pread) backend.\n");
	}

	ret = anyfs_open_image(ctx, image, flags);
	if (ret != ANYFS_OK) {
		fprintf(stderr, "anyfs_open_image(%s) failed: %d\n", image,
			ret);
		goto out;
	}

	AnyfsMount* mnt = NULL;
	ret = anyfs_mount(ctx, fstype, part, &mnt);
	if (ret != ANYFS_OK) {
		fprintf(stderr, "anyfs_mount(fs=%s, part=%u) failed: %d\n",
			fstype, part, ret);
		goto out;
	}

	printf("Mounted %s (part=%u, fs=%s) successfully.\n", image, part,
	       fstype);
	printf("Root directory listing:\n");

	AnyfsDir* dir = anyfs_opendir(mnt, "");
	if (!dir) {
		fprintf(stderr, "anyfs_opendir(\"/\") failed\n");
		goto umount;
	}

	AnyfsEntry entry;
	while (anyfs_readdir(dir, &entry) == ANYFS_OK) {
		const char* type_str;
		switch (entry.type) {
		case 4:
			type_str = "DIR ";
			break;
		case 8:
			type_str = "FILE";
			break;
		case 10:
			type_str = "LINK";
			break;
		default:
			type_str = "????";
			break;
		}
		printf("  [%s] %s (ino=%lu)\n", type_str, entry.name,
		       (unsigned long)entry.inode);
	}
	anyfs_closedir(dir);

	/* Test reading a file */
	anyfs_fd_t fd = anyfs_open(mnt, "hello.txt", 0);
	if (fd >= 0) {
		char buf[256] = {0};
		int64_t n = anyfs_read(mnt, fd, buf, sizeof(buf) - 1);
		if (n > 0)
			printf("\nhello.txt content: %s\n", buf);
		anyfs_close(mnt, fd);
	}

umount:
	anyfs_umount(mnt);
out:
	anyfs_destroy(ctx);
	return (ret == ANYFS_OK) ? 0 : 1;
}
