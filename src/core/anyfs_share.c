/*
 * share_spec.c — Implementation of the `--share` parsing helpers.
 */
#define _GNU_SOURCE
#include "anyfs_share.h"

#include <ctype.h>
#include <lkl.h>
#include <stdio.h>
#include <string.h>

void anyfs_share_auto_name(const char* path, char* out, size_t out_sz)
{
	if (out_sz == 0)
		return;
	size_t i, len = strlen(path);
	if (len >= out_sz)
		len = out_sz - 1;
	for (i = 0; i < len; i++)
		out[i] = (path[i] == '/') ? '_' : path[i];
	out[len] = '\0';
}

int anyfs_share_split(char* spec, const char** name_out, const char** path_out)
{
	char* eq = strchr(spec, '=');
	if (eq) {
		*eq = '\0';
		*name_out = spec;
		*path_out = eq + 1;
	} else {
		*name_out = NULL;
		*path_out = spec;
	}
	return 0;
}

void anyfs_share_warn_literal_key(const AnyfsPath* ap, const char* share_name)
{
	for (size_t i = 0; i < ap->n_comp; i++) {
		const char* q = ap->comp[i].query;
		if (!q || *q == '\0')
			continue;
		const char* kv = q;
		while (kv) {
			if (strncmp(kv, "key=", 4) == 0) {
				fprintf(stderr,
					"warning: --share %s uses literal "
					"'key=...' — "
					"the secret is visible in 'ps'. "
					"Use 'keyref=<envvar>' instead.\n",
					share_name);
				return;
			}
			kv = strchr(kv, '&');
			if (kv)
				kv++;
		}
	}
}

int anyfs_share_resolve(const char* spec, AnyfsDisk** disks, int n_disks,
			uint32_t enter_flags, char* name_out, size_t name_sz,
			char* lkl_out, size_t lkl_sz)
{
	/* 1. Copy spec to a mutable buffer for anyfs_share_split */
	char spec_copy[256];
	strncpy(spec_copy, spec, sizeof(spec_copy) - 1);
	spec_copy[sizeof(spec_copy) - 1] = '\0';

	const char* name_arg;
	const char* path_arg;
	anyfs_share_split(spec_copy, &name_arg, &path_arg);

	/* 2. Back-compat: bare integer → p<N> */
	char rebased[64];
	if (isdigit((unsigned char)path_arg[0])) {
		if (n_disks > 1) {
			fprintf(
			    stderr,
			    "error: bare integer share '%s' is only valid in "
			    "single-disk mode.\n",
			    path_arg);
			return -1;
		}
		snprintf(rebased, sizeof(rebased), "p%s", path_arg);
		path_arg = rebased;
		fprintf(stderr,
			"warning: --share %s treated as --share %s "
			"(use 'p<N>' to suppress this warning)\n",
			spec, rebased);
	}

	/* 3. Single-disk mode: auto-prefix missing disk<N>/ with disk0/ */
	char prefixed[256];
	if (n_disks == 1 && strncmp(path_arg, "disk", 4) != 0) {
		snprintf(prefixed, sizeof(prefixed), "disk0/%s", path_arg);
		path_arg = prefixed;
	}

	/* 4. Parse via path DSL */
	AnyfsPath ap;
	memset(&ap, 0, sizeof(ap));
	if (anyfs_path_parse(path_arg, &ap) < 0) {
		fprintf(stderr,
			"error: --share path '%s' is not a valid path DSL "
			"string.\n",
			path_arg);
		return -1;
	}

	/* 5. Multi-disk mode: path must have explicit disk<N>/ prefix */
	if (n_disks > 1 && !ap.disk_idx_set) {
		fprintf(stderr,
			"error: path '%s' must start with diskN/ in multi-disk "
			"mode (have %d images: disk0..disk%d).\n",
			spec, n_disks, n_disks - 1);
		anyfs_path_free(&ap);
		return -1;
	}

	int disk_idx = ap.disk_idx;

	/* 6. Validate disk_idx in range */
	if (disk_idx >= n_disks) {
		fprintf(stderr,
			"error: disk%d not registered (only disk0..disk%d "
			"available).\n",
			disk_idx, n_disks - 1);
		anyfs_path_free(&ap);
		return -1;
	}

	/* 7. Must have at least one path component */
	if (ap.n_comp == 0) {
		fprintf(
		    stderr,
		    "error: --share path '%s' has no partition component.\n",
		    path_arg);
		anyfs_path_free(&ap);
		return -1;
	}

	/* 8. Warn about literal 'key=...' credentials */
	anyfs_share_warn_literal_key(&ap, name_arg ? name_arg : path_arg);

	/* 9. Enter the partition chain */
	char lkl_path[ANYFS_LKL_PATH_MAX];
	int ret = anyfs_disk_enter_path(disks[disk_idx], ap.comp, ap.n_comp,
					enter_flags, lkl_path);
	if (ret < 0) {
		const char* reason =
		    anyfs_disk_fail_reason(disks[disk_idx], ap.comp[0].p);
		fprintf(
		    stderr,
		    "error: cannot enter %s: %s\n"
		    "Containers (LVM_PV, LUKS, nested partition table) "
		    "require\n"
		    "either a credential (`?keyref=`) or v3 support.\n"
		    "Use 'anyfs-lspart' to discover the canonical leaf path.\n",
		    path_arg, reason ? reason : lkl_strerror(ret));
		anyfs_path_free(&ap);
		return -1;
	}

	/* 10. Build canonical name for auto-name fallback */
	char canonical[160];
	int co = snprintf(canonical, sizeof(canonical), "disk%d", disk_idx);
	for (size_t ci = 0; ci < ap.n_comp; ci++) {
		int n = snprintf(canonical + co, sizeof(canonical) - co, "_p%u",
				 ap.comp[ci].p);
		if (n < 0 || (size_t)n >= sizeof(canonical) - co)
			break;
		co += n;
	}

	/* 11. Derive share name */
	if (name_arg && *name_arg) {
		strncpy(name_out, name_arg, name_sz - 1);
		name_out[name_sz - 1] = '\0';
	} else {
		anyfs_share_auto_name(canonical, name_out, name_sz);
	}

	strncpy(lkl_out, lkl_path, lkl_sz - 1);
	lkl_out[lkl_sz - 1] = '\0';

	anyfs_path_free(&ap);
	return 0;
}

int anyfs_share_open_disks(AnyfsDisk** disks_out, const char** images,
			   int n_images, uint32_t flags)
{
	for (int i = 0; i < n_images; i++) {
		const char* img = images[i];
		char img_clean[512];
		const char* qmark = strchr(img, '?');
		if (qmark) {
			size_t len = (size_t)(qmark - img);
			if (len >= sizeof(img_clean))
				len = sizeof(img_clean) - 1;
			memcpy(img_clean, img, len);
			img_clean[len] = '\0';
			img = img_clean;
		}

		AnyfsDisk* d = NULL;
		int rc = anyfs_disk_open(img, flags, &d);
		if (rc < 0 || !d) {
			fprintf(stderr,
				"Failed to open disk image '%s' (rc=%d)\n",
				images[i], rc);
			for (int j = 0; j < i; j++) {
				anyfs_disk_close(disks_out[j]);
				disks_out[j] = NULL;
			}
			return -1;
		}
		disks_out[i] = d;
		fprintf(stderr, "Opened disk%d: %s (id=%d)\n", i, img,
			anyfs_disk_id(d));
	}
	return 0;
}
