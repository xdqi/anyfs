/*
 * anyfs_disk_dump.c — Shared table printer for lspart + .partitions.
 *
 * v1: KIND populated by kindprobe; FSTYPE/LABEL/UUID are "?".
 */
#include "anyfs_disk_dump.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARTS 128

void anyfs_strbuf_init(AnyfsStrbuf* sb)
{
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
	sb->err = 0;
}

void anyfs_strbuf_free(AnyfsStrbuf* sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
	sb->err = 0;
}

char* anyfs_strbuf_detach(AnyfsStrbuf* sb, size_t* len_out)
{
	if (sb->err) {
		anyfs_strbuf_free(sb);
		return NULL;
	}
	char* r = sb->buf;
	if (len_out)
		*len_out = sb->len;
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
	return r;
}

static int strbuf_reserve(AnyfsStrbuf* sb, size_t need)
{
	if (sb->err)
		return -1;
	if (sb->cap - sb->len > need) /* room for `need` bytes + NUL */
		return 0;
	size_t ncap = sb->cap ? sb->cap : 256;
	while (ncap - sb->len <= need)
		ncap *= 2;
	char* nb = realloc(sb->buf, ncap);
	if (!nb) {
		sb->err = 1;
		return -1;
	}
	sb->buf = nb;
	sb->cap = ncap;
	return 0;
}

void anyfs_strbuf_printf(AnyfsStrbuf* sb, const char* fmt, ...)
{
	if (sb->err)
		return;
	va_list ap;
	va_start(ap, fmt);
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		sb->err = 1;
		return;
	}
	if (strbuf_reserve(sb, (size_t)n) < 0) {
		va_end(ap2);
		return;
	}
	vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap2);
	va_end(ap2);
	sb->len += (size_t)n;
}

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

void anyfs_dump_header(AnyfsStrbuf* sb)
{
	anyfs_strbuf_printf(sb, "%-18s %-10s %-7s %-7s %-12s %-12s\n", "PATH",
			    "SIZE", "KIND", "FSTYPE", "LABEL", "UUID");
}

void anyfs_dump_disk(AnyfsStrbuf* sb, AnyfsDisk* d, int disk_idx)
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
