/*
 * test_session_ro_mount.c — a read-only mount must not write to the disk,
 * even when the session itself is writable.
 *
 * Regression: LKL's virtio-blk never marks a device read-only, and ext4
 * decides whether it may record an error in the superblock by checking
 * bdev_read_only(), not sb_rdonly(). So a filesystem error on a read-only
 * mount (here: a bad inode checksum) made ext4 write the superblock back
 * (s_error_count, s_state |= EXT4_ERROR_FS, first/last error fields) to a
 * disk the caller asked to leave untouched.
 *
 * Also covered: the read-only marking must not outlive the mount. Leaving
 * and re-entering the same partition read-write must give a writable
 * filesystem.
 *
 * The image is generated with mkfs.ext4 (metadata_csum) and debugfs; the
 * inode of /docs is zeroed so the first lookup fails its checksum. Skipped
 * (exit 77) when e2fsprogs is missing. Raw backend, writable session.
 */
#include "anyfs.h"

#include <fcntl.h>
#include <lkl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMG_SIZE (8u << 20)
#define BLOCK_SIZE 4096u
#define INODE_SIZE 256u
#define TOOLS_PATH "export PATH=\"$PATH:/usr/sbin:/sbin\"; "

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			exit(1);                                               \
		}                                                              \
	} while (0)

static char dir[64];
static char img[96];

static void cleanup(void)
{
	char cmd[160];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	(void)system(cmd);
}

/* ext4 image with /docs/readme.txt whose /docs inode is zeroed. */
static void make_image(void)
{
	snprintf(dir, sizeof(dir), "/tmp/anyfs-romount-XXXXXX");
	CHECK(mkdtemp(dir), "mkdtemp");
	atexit(cleanup);

	char path[128];
	snprintf(path, sizeof(path), "%s/root", dir);
	CHECK(mkdir(path, 0755) == 0, "mkdir root");
	snprintf(path, sizeof(path), "%s/root/docs", dir);
	CHECK(mkdir(path, 0755) == 0, "mkdir docs");
	snprintf(path, sizeof(path), "%s/root/docs/readme.txt", dir);
	FILE* f = fopen(path, "w");
	CHECK(f && fputs("hello\n", f) >= 0 && fclose(f) == 0, "write file");

	snprintf(img, sizeof(img), "%s/ext4.img", dir);
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 TOOLS_PATH "mkfs.ext4 -q -F -b %u -I %u -O metadata_csum "
			    "-e continue -d '%s/root' '%s' %uk",
		 BLOCK_SIZE, INODE_SIZE, dir, img, IMG_SIZE >> 10);
	CHECK(system(cmd) == 0, "mkfs.ext4");

	snprintf(cmd, sizeof(cmd),
		 TOOLS_PATH "debugfs -R 'imap /docs' '%s' 2>/dev/null", img);
	FILE* p = popen(cmd, "r");
	CHECK(p, "debugfs");
	unsigned long block = 0;
	unsigned offset = 0;
	char line[160];
	while (fgets(line, sizeof(line), p))
		if (sscanf(line, " located at block %lu, offset 0x%x", &block,
			   &offset) == 2)
			break;
	pclose(p);
	CHECK(block != 0, "debugfs imap /docs: no location");

	int fd = open(img, O_WRONLY);
	unsigned char zero[INODE_SIZE] = {0};
	CHECK(fd >= 0 &&
		  pwrite(fd, zero, sizeof(zero),
			 (off_t)block * BLOCK_SIZE + offset) == sizeof(zero),
	      "zero /docs inode");
	close(fd);
}

static unsigned char* read_image(void)
{
	unsigned char* buf = malloc(IMG_SIZE);
	FILE* f = fopen(img, "rb");
	CHECK(buf && f && fread(buf, 1, IMG_SIZE, f) == IMG_SIZE, "read image");
	fclose(f);
	return buf;
}

int main(void)
{
	if (system(TOOLS_PATH "command -v mkfs.ext4 >/dev/null && "
			      "command -v debugfs >/dev/null") != 0) {
		printf("skip: mkfs.ext4/debugfs not found\n");
		return 77;
	}
	make_image();
	unsigned char* before = read_image();

	CHECK(anyfs_kernel_init(NULL) == 0, "kernel init");

	AnyfsSession* s = NULL;
	CHECK(anyfs_session_open(img, ANYFS_BACKEND_RAW, &s) == 0, "open: %s",
	      anyfs_get_last_error() ? anyfs_get_last_error() : "?");

	char mnt[ANYFS_LKL_PATH_MAX], path[ANYFS_LKL_PATH_MAX + 16];
	CHECK(anyfs_session_enter(s, 0, ANYFS_MOUNT_RDONLY, mnt) == 0,
	      "enter read-only");
	snprintf(path, sizeof(path), "%s/docs", mnt);
	long rc = lkl_sys_access(path, 0);
	printf("access %s -> %ld\n", path, rc);
	CHECK(rc < 0, "the zeroed /docs inode must fail its checksum");
	CHECK(anyfs_session_leave(s, 0) == 0, "leave");

	unsigned char* after = read_image();
	size_t first = 0;
	while (first < IMG_SIZE && before[first] == after[first])
		first++;
	CHECK(first == IMG_SIZE,
	      "a read-only mount wrote to the disk (first changed byte at "
	      "offset %zu)",
	      first);

	/* The same device, now read-write: it must not still be read-only. */
	CHECK(anyfs_session_enter(s, 0, 0, mnt) == 0, "enter read-write");
	snprintf(path, sizeof(path), "%s/new.txt", mnt);
	rc = lkl_sys_open(path, LKL_O_CREAT | LKL_O_WRONLY, 0644);
	printf("create %s -> %ld\n", path, rc);
	CHECK(rc >= 0, "create on the read-write mount");
	lkl_sys_close(rc);

	anyfs_session_close(s);
	free(before);
	free(after);
	printf("ok\n");
	return 0;
}
