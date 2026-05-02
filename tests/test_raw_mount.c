/*
 * Test: mount a raw disk image via anyfs + LKL, list root directory.
 * Usage: test_raw_mount [--gio|--raw] <image> <fstype> <part>
 */
#include "anyfs.h"
#include <lkl/asm-generic/fcntl.h>
#include <lkl/linux/mount.h>
#include <lkl/linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	uint32_t backend_flag = 0;
	int argoff = 1;

	if (argc > 1 && strcmp(argv[1], "--gio") == 0) {
		backend_flag = ANYFS_BACKEND_GIO;
		argoff = 2;
	} else if (argc > 1 && strcmp(argv[1], "--raw") == 0) {
		backend_flag = ANYFS_BACKEND_RAW;
		argoff = 2;
	}

	if (argc - argoff < 3) {
		fprintf(stderr,
			"usage: %s [--gio|--raw] <image> <fstype> <part>\n",
			argv[0]);
		return 1;
	}

	const char* image = argv[argoff];
	const char* fstype = argv[argoff + 1];
	uint32_t part = (uint32_t)atoi(argv[argoff + 2]);

	if (anyfs_kernel_init(NULL) < 0) {
		fprintf(stderr, "kernel init failed\n");
		return 1;
	}

	uint32_t flags = ANYFS_DISK_READONLY | backend_flag;
	int disk_id = anyfs_disk_add(image, flags);
	if (disk_id < 0) {
		fprintf(stderr, "anyfs_disk_add(%s) failed\n", image);
		anyfs_kernel_halt();
		return 1;
	}

	char mount_point[32];
	long ret = lkl_mount_dev(disk_id, part, fstype, LKL_MS_RDONLY, NULL,
				 mount_point, sizeof(mount_point));
	if (ret) {
		fprintf(stderr, "lkl_mount_dev failed: %ld\n", ret);
		anyfs_disk_remove(disk_id);
		anyfs_kernel_halt();
		return 1;
	}

	printf("Mounted %s (part=%u, fs=%s) at %s\n", image, part, fstype,
	       mount_point);
	printf("Root directory listing:\n");

	int err;
	struct lkl_dir* dir = lkl_opendir(mount_point, &err);
	if (dir) {
		struct lkl_linux_dirent64* de;
		while ((de = lkl_readdir(dir)) != NULL) {
			const char* type_str;
			switch (de->d_type) {
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
			printf("  [%s] %s (ino=%lu)\n", type_str, de->d_name,
			       (unsigned long)de->d_ino);
		}
		lkl_closedir(dir);
	}

	/* Test reading a file */
	char fpath[256];
	snprintf(fpath, sizeof(fpath), "%s/hello.txt", mount_point);
	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd >= 0) {
		char buf[256] = {0};
		long n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
		if (n > 0)
			printf("\nhello.txt content: %s\n", buf);
		lkl_sys_close(fd);
	}

	lkl_umount_dev(disk_id, part, 0, 1000);
	anyfs_disk_remove(disk_id);
	anyfs_kernel_halt();
	return 0;
}
