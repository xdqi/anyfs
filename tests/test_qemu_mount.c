/*
 * Test: mount a qcow2/vmdk/vdi/raw image using QEMU block backend + LKL
 *
 * Usage: test_qemu_mount <image_path> [fs_type]
 */
#include <lkl.h>
#include <lkl_host.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in qemu_blk_backend.c */
extern struct lkl_dev_blk_ops qemu_blk_ops;
extern int qemu_blk_open(const char* image_path, int readonly,
			 struct lkl_disk* disk_out);
extern void qemu_blk_close(struct lkl_disk* disk);

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <image_path> [fs_type]\n", argv[0]);
		return 1;
	}

	const char* image_path = argv[1];
	const char* fs_type = argc > 2 ? argv[2] : "ext4";

	struct lkl_disk disk = {0};
	if (qemu_blk_open(image_path, 1, &disk) < 0) {
		return 1;
	}

	lkl_init(&lkl_host_ops);
	int disk_id = lkl_disk_add(&disk);
	if (disk_id < 0) {
		fprintf(stderr, "lkl_disk_add failed: %d\n", disk_id);
		return 1;
	}

	lkl_start_kernel("mem=64M");

	char mount_point[32];
	long ret = lkl_mount_dev(disk_id, 0, fs_type, LKL_MS_RDONLY, NULL,
				 mount_point, sizeof(mount_point));
	if (ret < 0) {
		fprintf(stderr, "lkl_mount_dev failed: %ld\n", ret);
		lkl_sys_halt();
		return 1;
	}

	printf("Mounted %s (%s) at %s\n", image_path, fs_type, mount_point);

	/* List root directory */
	int dir_fd =
	    lkl_sys_open(mount_point, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
	if (dir_fd >= 0) {
		char buf[4096];
		long nread;
		while ((nread = lkl_sys_getdents64(
			    dir_fd, (struct lkl_linux_dirent64*)buf,
			    sizeof(buf))) > 0) {
			long pos = 0;
			while (pos < nread) {
				struct lkl_linux_dirent64* de =
				    (struct lkl_linux_dirent64*)(buf + pos);
				printf("  %s\n", de->d_name);
				pos += de->d_reclen;
			}
		}
		lkl_sys_close(dir_fd);
	}

	lkl_umount_dev(disk_id, 0, 0, 1000);
	lkl_sys_halt();
	qemu_blk_close(&disk);
	printf("OK\n");
	return 0;
}
