/*
 * anyfs_format.c — Shared table printer for lspart + .partitions.
 *
 * v1: KIND populated by kindprobe; FSTYPE/LABEL/UUID are "?".
 */
#include "anyfs_format.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define MAX_PARTS 128

static void human_size(uint64_t bytes, char* out, size_t cap)
{
	static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 5) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(out, cap, "%" PRIu64 " %s", bytes, units[u]);
	else
		snprintf(out, cap, "%.1f %s", v, units[u]);
}

void anyfs_format_header(AnyfsStrbuf* sb)
{
	anyfs_strbuf_printf(sb, "%-18s %-10s %-7s %-7s %-12s %-12s\n", "PATH",
			    "SIZE", "KIND", "FSTYPE", "LABEL", "UUID");
}

void anyfs_format_disk(AnyfsStrbuf* sb, AnyfsDisk* d, int disk_idx)
{
	if (!d)
		return;
	AnyfsPartInfo parts[MAX_PARTS];
	size_t got = 0;
	int n = anyfs_disk_list(d, parts, MAX_PARTS, &got);
	if (n <= 0)
		return;

	for (int i = 0; i < n; i++) {
		char path[32];
		snprintf(path, sizeof(path), "disk%d/p%u", disk_idx,
			 parts[i].index);
		char sz[24];
		human_size(parts[i].size_bytes, sz, sizeof(sz));
		const char* fs = parts[i].fstype[0] ? parts[i].fstype : "?";
		const char* label = parts[i].label[0] ? parts[i].label : "?";
		const char* uuid = parts[i].uuid[0] ? parts[i].uuid : "?";
		anyfs_strbuf_printf(
		    sb, "%-18s %-10s %-7s %-7s %-12s %-12s\n", path, sz,
		    anyfs_partkind_name(parts[i].kind), fs, label, uuid);
	}
}
