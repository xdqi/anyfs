/* Smoke test: probe a real disk image and print TYPE/LABEL/UUID.
 *
 * Mirrors src/core/kindprobe.c:380-450 on the non-emscripten branch
 * (mingw has no /proc/self/fd, so we use blkid_new_probe_from_filename).
 */
#include <blkid/blkid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "/tmp/x";

	fprintf(stderr, "smoke: probing %s\n", path);
	fflush(stderr);

	blkid_probe pr = blkid_new_probe_from_filename(path);
	if (!pr) {
		fprintf(stderr, "new_probe_from_filename(%s) returned NULL\n",
			path);
		return 1;
	}
	fprintf(stderr, "smoke: probe allocated\n");
	fflush(stderr);

	blkid_probe_enable_superblocks(pr, 1);
	blkid_probe_set_superblocks_flags(
	    pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID);

	fprintf(stderr, "smoke: starting do_safeprobe\n");
	fflush(stderr);

	int rc = blkid_do_safeprobe(pr);
	fprintf(stderr, "smoke: do_safeprobe rc=%d\n", rc);
	fflush(stderr);

	const char* val = NULL;
	size_t vlen = 0;
	if (blkid_probe_lookup_value(pr, "TYPE", &val, &vlen) == 0)
		printf("TYPE=%.*s\n", (int)vlen, val);
	if (blkid_probe_lookup_value(pr, "LABEL", &val, &vlen) == 0)
		printf("LABEL=%.*s\n", (int)vlen, val);
	if (blkid_probe_lookup_value(pr, "UUID", &val, &vlen) == 0)
		printf("UUID=%.*s\n", (int)vlen, val);
	fflush(stdout);

	fprintf(stderr, "smoke: freeing probe\n");
	fflush(stderr);
	blkid_free_probe(pr);
	fprintf(stderr, "smoke: done\n");
	return 0;
}
