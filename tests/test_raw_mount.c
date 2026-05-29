/*
 * Test: mount a raw disk image via anyfs + LKL, list root directory.
 * Usage: test_raw_mount [--gio|--raw] <image> <fstype> <part>
 */
#include "anyfs.h"
#include "anyfs_backend.h"
#include <lkl/asm-generic/fcntl.h>
#include <lkl/linux/capability.h>
#include <lkl/linux/mount.h>
#include <lkl/linux/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	uint32_t backend_flag = 0;
	int argoff = 1;
	int impersonate_uid = -1;

	while (argc > argoff) {
		if (strcmp(argv[argoff], "--gio") == 0) {
			backend_flag = ANYFS_BACKEND_GIO;
			argoff++;
		} else if (strcmp(argv[argoff], "--raw") == 0) {
			backend_flag = ANYFS_BACKEND_RAW;
			argoff++;
		} else if (strcmp(argv[argoff], "--uid") == 0 &&
			   argc > argoff + 1) {
			impersonate_uid = atoi(argv[argoff + 1]);
			argoff += 2;
		} else {
			break;
		}
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

	uint32_t flags = ANYFS_SESSION_READONLY | backend_flag;
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

	/* Test reading a file: 4th positional arg picks file name in root. */
	const char* fname =
	    (argc - argoff >= 4) ? argv[argoff + 3] : "hello.txt";
	char fpath[256];
	snprintf(fpath, sizeof(fpath), "%s/%s", mount_point, fname);
	{
		struct lkl_stat st;
		long sr = lkl_sys_stat(fpath, &st);
		if (sr == 0)
			printf("stat %s: mode=0%o uid=%u gid=%u size=%lld\n",
			       fpath, (unsigned)st.st_mode, (unsigned)st.st_uid,
			       (unsigned)st.st_gid, (long long)st.st_size);
		else
			printf("stat %s: failed=%ld\n", fpath, sr);
	}
	if (impersonate_uid >= 0) {
		long prev = lkl_sys_setfsuid((uint32_t)impersonate_uid);
		long now =
		    lkl_sys_setfsuid((uint32_t)impersonate_uid); /* readback */
		printf("setfsuid(%d) prev=%ld now=%ld\n", impersonate_uid, prev,
		       now);
	}

	/* Probe capabilities to see if CAP_DAC_OVERRIDE / CAP_DAC_READ_SEARCH
	 * is set */
	{
		struct __lkl__user_cap_header_struct hdr = {
		    .version = _LKL_LINUX_CAPABILITY_VERSION_3,
		    .pid = 0,
		};
		struct __lkl__user_cap_data_struct data[2] = {{0}};
		long r = lkl_sys_capget(&hdr, data);
		printf(
		    "capget: rc=%ld effective=%08x,%08x permitted=%08x,%08x\n",
		    r, data[0].effective, data[1].effective, data[0].permitted,
		    data[1].permitted);
		/* CAP_DAC_OVERRIDE = 1, CAP_DAC_READ_SEARCH = 2, CAP_FOWNER = 3
		 */
		printf("  CAP_DAC_OVERRIDE=%d CAP_DAC_READ_SEARCH=%d "
		       "CAP_FOWNER=%d\n",
		       !!(data[0].effective & (1u << 1)),
		       !!(data[0].effective & (1u << 2)),
		       !!(data[0].effective & (1u << 3)));
	}

	/* Try access() to see what permission check says */
	{
		long r = lkl_sys_access(fpath, 4 /* R_OK */);
		printf("access(R_OK) on %s: rc=%ld\n", fpath, r);
	}

	/* Stat each path component as fsuid=X, to see where traversal fails */
	{
		const char* probe_paths[] = {"/", "/mnt", mount_point, NULL};
		for (int i = 0; probe_paths[i]; i++) {
			struct lkl_stat st;
			long sr = lkl_sys_stat(probe_paths[i], &st);
			if (sr == 0)
				printf("  stat %s: mode=0%o uid=%u gid=%u\n",
				       probe_paths[i], (unsigned)st.st_mode,
				       (unsigned)st.st_uid,
				       (unsigned)st.st_gid);
			else
				printf("  stat %s: failed=%ld\n",
				       probe_paths[i], sr);
			/* Probe access(X_OK) too */
			long ar = lkl_sys_access(probe_paths[i], 1 /* X_OK */);
			printf("    access(X_OK)=%ld\n", ar);
		}
	}

	/* Try opening via openat with AT_FDCWD then with O_PATH on parent */
	{
		long fd_dir = lkl_sys_open(mount_point, LKL_O_RDONLY, 0);
		printf("open(mount_point, RDONLY): %ld\n", fd_dir);
		if (fd_dir >= 0)
			lkl_sys_close(fd_dir);
	}

	/* Chmod /mnt and the mount_point to 0755 to allow non-root traversal */
	{
		/* switch back to root first so chmod works */
		long old_fs = lkl_sys_setfsuid(0);
		long m1 = lkl_sys_chmod("/mnt", 0755);
		long m2 = lkl_sys_chmod(mount_point, 0755);
		printf("chmod /mnt 0755: %ld; chmod %s 0755: %ld\n", m1,
		       mount_point, m2);
		if (impersonate_uid >= 0)
			lkl_sys_setfsuid((uint32_t)impersonate_uid);
		else
			lkl_sys_setfsuid((uint32_t)old_fs);

		long fd2 = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
		if (fd2 < 0)
			printf("AFTER chmod: open %s -> %ld\n", fpath, fd2);
		else {
			char b[64] = {0};
			long n = lkl_sys_read(fd2, b, sizeof(b) - 1);
			printf("AFTER chmod: open %s -> fd=%ld read=%ld "
			       "content=%.*s\n",
			       fpath, fd2, n, (int)n, b);
			lkl_sys_close(fd2);
		}
	}

	printf("\nOpening %s ... ", fpath);
	fflush(stdout);
	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd < 0) {
		printf("FAIL: lkl_sys_open returned %ld\n", fd);
	} else {
		char buf[256] = {0};
		long n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
		printf("OPEN ok (fd=%ld), read=%ld\n", fd, n);
		if (n > 0)
			printf("content (%ld bytes): %.*s\n", n, (int)n, buf);
		else if (n < 0)
			printf("read error: %ld\n", n);
		lkl_sys_close(fd);
	}

	lkl_umount_dev(disk_id, part, 0, 1000);
	anyfs_disk_remove(disk_id);
	anyfs_kernel_halt();
	return 0;
}
