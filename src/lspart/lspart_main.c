/*
 * anyfs-lspart — Standalone discovery tool.
 *
 * Open one or more images, list partitions cheaply (no mounts, no
 * LUKS prompt), print a unified table. The PATH column is the
 * canonical disk<N>/p<M> form that surface --share flags accept.
 */
#define _GNU_SOURCE

#include "anyfs.h"
#include "anyfs_disk_dump.h"
#include "anyfs_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE* f, const char* prog)
{
	fprintf(f,
		"Usage: %s [--json] [--help] <image>[?<query>] [<image>...]\n"
		"\n"
		"Open each image, list partitions, print a unified table.\n"
		"PATH column is the canonical disk<N>/p<M> form (drops into\n"
		"    anyfs-fuse -o part=, anyfs-ksmbd --share, anyfs-nfsd "
		"--share).\n"
		"\n"
		"v1 limitations:\n"
		"  --json is reserved (not yet implemented).\n"
		"  FSTYPE/LABEL/UUID columns show '?' (v2 adds libblkid).\n",
		prog);
}

int main(int argc, char** argv)
{
	int json = 0;
	const char* images[16];
	int n_images = 0;

	for (int i = 1; i < argc; i++) {
		const char* a = argv[i];
		if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
			usage(stdout, argv[0]);
			return 0;
		}
		if (strcmp(a, "--json") == 0) {
			json = 1;
			continue;
		}
		if (a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "unknown flag: %s\n", a);
			usage(stderr, argv[0]);
			return 2;
		}
		if (n_images >= (int)(sizeof(images) / sizeof(images[0]))) {
			fprintf(stderr, "too many images\n");
			return 2;
		}
		images[n_images++] = a;
	}

	if (n_images == 0) {
		usage(stderr, argv[0]);
		return 2;
	}

	if (json) {
		/* v1: stub. Honoured at --help wording; surface explicitly. */
		fprintf(stderr, "warning: --json not implemented in v1; "
				"falling back to text\n");
	}

	AnyfsKernelOpts opts = {0};
	if (anyfs_kernel_init(&opts) < 0) {
		fprintf(stderr, "anyfs-lspart: kernel init failed\n");
		return 1;
	}

	AnyfsDisk* disks[16] = {0};
	int n_open = 0;
	int rc = 0;
	for (int i = 0; i < n_images; i++) {
		/* Strip optional `?...` query off the image path for now. v2
		 * will route it through credential resolution. */
		const char* path = images[i];
		char clean[512];
		const char* q = strchr(path, '?');
		if (q) {
			size_t len = (size_t)(q - path);
			if (len >= sizeof(clean))
				len = sizeof(clean) - 1;
			memcpy(clean, path, len);
			clean[len] = '\0';
			path = clean;
		}
		AnyfsDisk* d = NULL;
		if (anyfs_disk_open(path, ANYFS_DISK_READONLY, &d) < 0 || !d) {
			fprintf(stderr, "anyfs-lspart: failed to open %s\n",
				images[i]);
			rc = 1;
			continue;
		}
		disks[n_open++] = d;
	}

	AnyfsStrbuf sb;
	anyfs_strbuf_init(&sb);
	anyfs_dump_header(&sb);
	int disk_idx = 0;
	for (int i = 0; i < n_open; i++) {
		anyfs_dump_disk(&sb, disks[i], disk_idx++);
	}
	if (sb.err) {
		fprintf(stderr,
			"anyfs-lspart: out of memory building output\n");
		rc = 1;
	} else if (sb.len) {
		fwrite(sb.buf, 1, sb.len, stdout);
	}
	anyfs_strbuf_free(&sb);

	for (int i = 0; i < n_open; i++)
		anyfs_disk_close(disks[i]);

	return rc;
}
