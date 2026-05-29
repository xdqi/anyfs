#ifndef JSONW_H
#define JSONW_H
/*
 * jsonw.h — minimal JSON writer for C glue layers
 *
 * Zero dependencies beyond <stdio.h>. Designed for C→JS bridges that
 * serialise structured data (disk partitions, directory entries, stat
 * results) in one amortised call rather than crossing the wasm↔JS or
 * N-API boundary per entry.
 *
 * Buffer-size protocol: if the output buffer is too small, jw_finish
 * returns a NEGATIVE number whose absolute value is the byte count that
 * should have been written. The caller retries with a larger buffer.
 *
 * Example:
 *   JsonW w;
 *   jw_init(&w, buf, cap);
 *   jw_putc(&w, '{');
 *   jw_kv_str(&w, "name", "value", 0);
 *   jw_putc(&w, '}');
 *   int n = jw_finish(&w, buf, cap);
 *   if (n < 0) { /-* buffer too small: need abs(n) bytes *-/ }
 */

#include <stdint.h>
#include <stdio.h>

typedef struct {
	char* buf;
	size_t cap;
	size_t len;
	int overflow; /* 1 if we tried to write past cap */
} JsonW;

static void jw_init(JsonW* w, char* buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->overflow = 0;
}

static void jw_putc(JsonW* w, char c)
{
	if (w->len + 1 < w->cap)
		w->buf[w->len] = c;
	else
		w->overflow = 1;
	w->len++;
}

static void jw_puts(JsonW* w, const char* s)
{
	while (*s)
		jw_putc(w, *s++);
}

static void jw_escape_str(JsonW* w, const char* s)
{
	jw_putc(w, '"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			jw_putc(w, '\\');
			jw_putc(w, c);
		} else if (c == '\n') {
			jw_putc(w, '\\');
			jw_putc(w, 'n');
		} else if (c == '\r') {
			jw_putc(w, '\\');
			jw_putc(w, 'r');
		} else if (c == '\t') {
			jw_putc(w, '\\');
			jw_putc(w, 't');
		} else if (c < 0x20) {
			char tmp[8];
			snprintf(tmp, sizeof(tmp), "\\u%04x", c);
			jw_puts(w, tmp);
		} else {
			jw_putc(w, c);
		}
	}
	jw_putc(w, '"');
}

static void jw_int(JsonW* w, long long v)
{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%lld", v);
	jw_puts(w, tmp);
}

static void jw_uint(JsonW* w, unsigned long long v)
{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%llu", v);
	jw_puts(w, tmp);
}

static void jw_kv_str(JsonW* w, const char* k, const char* v,
		      int trailing_comma)
{
	jw_escape_str(w, k);
	jw_putc(w, ':');
	jw_escape_str(w, v);
	if (trailing_comma)
		jw_putc(w, ',');
}

static void jw_kv_int(JsonW* w, const char* k, long long v, int trailing_comma)
{
	jw_escape_str(w, k);
	jw_putc(w, ':');
	jw_int(w, v);
	if (trailing_comma)
		jw_putc(w, ',');
}

static void jw_kv_uint(JsonW* w, const char* k, unsigned long long v,
		       int trailing_comma)
{
	jw_escape_str(w, k);
	jw_putc(w, ':');
	jw_uint(w, v);
	if (trailing_comma)
		jw_putc(w, ',');
}

/* Returns bytes that should have been written (excluding NUL).
 * If overflow: caller sees negative -(bytes+1). */
static int jw_finish(JsonW* w, char* buf, size_t cap)
{
	(void)buf;
	(void)cap;
	if (w->overflow || w->len + 1 > w->cap) {
		return -(int)(w->len + 1);
	}
	w->buf[w->len] = '\0';
	return (int)w->len;
}

#endif /* JSONW_H */
