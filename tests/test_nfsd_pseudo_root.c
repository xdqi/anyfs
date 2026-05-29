/*
 * Test: NFSv4 pseudo-fs bind-mount logic used by anyfs-nfsd.
 *
 * anyfs-nfsd needs each --share to be visible at /<name> (not at the
 * /lklmnt/anyfs_d<N>_p<M> path the disk-enter returns). It does that by
 * bind-mounting the share root onto /<name> inside LKL. This test
 * exercises just that piece — no nfsd, no host_proxy, no NFS client —
 * so the pseudo-root semantics can be validated without depending on
 * an NFS test environment.
 *
 * The test:
 *   1. boots LKL,
 *   2. opens an image and enters disk0/p<part>,
 *   3. mkdir /<name> in the LKL rootfs and bind-mounts the share onto it,
 *   4. lists /<name> and confirms it contains the same entries as the
 *      original lkl_path (i.e. the partition root, not a child dir).
 *
 * Usage:
 *   test_nfsd_pseudo_root <image> <part-N> <share-name>
 *
 *   <part-N>      partition index (e.g. 1 for disk0/p1)
 *   <share-name>  what to bind-mount to / and list (e.g. "root")
 *
 * Exit codes:
 *   0  bind path lists the partition root entries
 *   1  setup failure (init / open / enter / mkdir / mount)
 *   2  listings diverge — bind path is NOT showing the share root
 */
#include "anyfs.h"
#include <lkl.h>
#include <lkl_host.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 64

struct entries {
	char names[MAX_ENTRIES][96];
	int n;
};

static int snapshot_dir(const char* path, struct entries* out)
{
	out->n = 0;
	int err = 0;
	struct lkl_dir* dir = lkl_opendir(path, &err);
	if (!dir) {
		fprintf(stderr, "lkl_opendir(%s) failed: %d\n", path, err);
		return -1;
	}
	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL && out->n < MAX_ENTRIES) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		strncpy(out->names[out->n], de->d_name,
			sizeof(out->names[out->n]) - 1);
		out->names[out->n][sizeof(out->names[out->n]) - 1] = '\0';
		out->n++;
	}
	lkl_closedir(dir);
	return 0;
}

static int entries_have(const struct entries* e, const char* name)
{
	for (int i = 0; i < e->n; i++)
		if (strcmp(e->names[i], name) == 0)
			return 1;
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <image> <part-N> <share-name>\n"
			"  e.g.  %s debian.qcow2 1 root\n",
			argv[0], argv[0]);
		return 1;
	}
	const char* image = argv[1];
	int part = atoi(argv[2]);
	const char* name = argv[3];

	if (anyfs_kernel_init(NULL) < 0) {
		fprintf(stderr, "anyfs_kernel_init failed\n");
		return 1;
	}

	AnyfsSession* disk = NULL;
	if (anyfs_session_open(image, ANYFS_SESSION_READONLY, &disk) < 0 ||
	    !disk) {
		fprintf(stderr, "anyfs_session_open(%s) failed\n", image);
		anyfs_kernel_halt();
		return 1;
	}

	AnyfsPathComp comp = {.p = (uint32_t)part};
	char lkl_path[64];
	int r = anyfs_session_enter_path(disk, &comp, 1, ANYFS_SESSION_READONLY,
					 lkl_path);
	if (r < 0) {
		fprintf(stderr, "anyfs_session_enter_path(p%d) failed: %d\n",
			part, r);
		anyfs_session_close(disk);
		anyfs_kernel_halt();
		return 1;
	}
	printf("Share enters at: %s\n", lkl_path);

	char bind_path[80];
	snprintf(bind_path, sizeof(bind_path), "/%s", name);

	int mret = lkl_sys_mkdir(bind_path, 0755);
	if (mret < 0 && mret != -LKL_EEXIST) {
		fprintf(stderr, "lkl_sys_mkdir(%s): %s\n", bind_path,
			lkl_strerror(mret));
		anyfs_session_close(disk);
		anyfs_kernel_halt();
		return 1;
	}

	mret = lkl_sys_mount(lkl_path, bind_path, NULL, LKL_MS_BIND, NULL);
	if (mret < 0) {
		fprintf(stderr, "bind %s -> %s: %s\n", lkl_path, bind_path,
			lkl_strerror(mret));
		anyfs_session_close(disk);
		anyfs_kernel_halt();
		return 1;
	}
	printf("Bound %s -> %s\n", bind_path, lkl_path);

	struct entries src = {0};
	struct entries dst = {0};
	if (snapshot_dir(lkl_path, &src) < 0 ||
	    snapshot_dir(bind_path, &dst) < 0) {
		anyfs_session_close(disk);
		anyfs_kernel_halt();
		return 1;
	}

	printf("\nShare root (%s) has %d entries:\n", lkl_path, src.n);
	for (int i = 0; i < src.n; i++)
		printf("  %s\n", src.names[i]);

	printf("\nBind path (%s) has %d entries:\n", bind_path, dst.n);
	for (int i = 0; i < dst.n; i++)
		printf("  %s\n", dst.names[i]);

	/* The bind path must have the same entry set as the original mount —
	 * that's the whole point of MS_BIND. If they diverge, the kernel walked
	 * a real subdirectory named <share-name> instead of the bind mount. */
	int verdict = 0;
	if (src.n != dst.n) {
		fprintf(stderr, "\nFAIL: entry count differs (src=%d dst=%d)\n",
			src.n, dst.n);
		verdict = 2;
	}
	for (int i = 0; i < src.n && verdict == 0; i++) {
		if (!entries_have(&dst, src.names[i])) {
			fprintf(stderr,
				"\nFAIL: '%s' present in share root but not in "
				"bind path\n",
				src.names[i]);
			verdict = 2;
		}
	}

	if (verdict == 0)
		printf("\nPASS: bind path mirrors the share root.\n");

	anyfs_session_close(disk);
	anyfs_kernel_halt();
	return verdict;
}
