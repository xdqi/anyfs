/*
 * anyfs_strbuf.c — Simple appendable string buffer.
 */
#include "anyfs_strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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
