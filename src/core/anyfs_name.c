/*
 * anyfs_name.c — file-name escape for the JavaScript side; see anyfs_name.h.
 */
#include "anyfs_name.h"

#include <stdint.h>
#include <string.h>

/* Length of the strict UTF-8 sequence at s (1-4), or 0 if s does not start
 * one. Strict: no overlong forms, no surrogates, nothing above U+10FFFF. */
static size_t utf8_seq(const unsigned char* s)
{
	unsigned c = s[0];

	if (c < 0x80)
		return 1;
	if (c >= 0xC2 && c <= 0xDF)
		return (s[1] & 0xC0) == 0x80 ? 2 : 0;
	if (c >= 0xE0 && c <= 0xEF) {
		unsigned lo = c == 0xE0 ? 0xA0 : 0x80;
		unsigned hi = c == 0xED ? 0x9F : 0xBF;
		return s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80 ? 3
									 : 0;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		unsigned lo = c == 0xF0 ? 0x90 : 0x80;
		unsigned hi = c == 0xF4 ? 0x8F : 0xBF;
		return s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80 &&
			       (s[3] & 0xC0) == 0x80
			   ? 4
			   : 0;
	}
	return 0;
}

/* U+EF80..U+EFFF is EE BE 80..BF and EE BF 80..BF. */
static int is_escape_char(const unsigned char* s)
{
	return s[0] == 0xEE && (s[1] == 0xBE || s[1] == 0xBF) &&
	       (s[2] & 0xC0) == 0x80;
}

int anyfs_name_escape(const char* in, char* out, size_t cap)
{
	const unsigned char* s = (const unsigned char*)in;
	size_t o = 0;

	if (!out || cap == 0)
		return -1;
	while (*s) {
		size_t n = utf8_seq(s);
		if (n && !(n == 3 && is_escape_char(s))) {
			if (o + n >= cap)
				goto overflow;
			memcpy(out + o, s, n);
			o += n;
			s += n;
			continue;
		}
		/* One byte: invalid here, or part of an escape character. */
		n = n ? n : 1;
		for (size_t k = 0; k < n; k++, s++) {
			unsigned b = *s;
			if (o + 3 >= cap)
				goto overflow;
			out[o++] = (char)0xEE;
			out[o++] = (char)(b >= 0xC0 ? 0xBF : 0xBE);
			out[o++] = (char)(0x80 | (b & 0x3F));
		}
	}
	out[o] = '\0';
	return (int)o;

overflow:
	out[0] = '\0';
	return -1;
}

int anyfs_name_unescape(const char* in, char* out, size_t cap)
{
	const unsigned char* s = (const unsigned char*)in;
	size_t o = 0;

	if (!out || cap == 0)
		return -1;
	while (*s) {
		if (o + 1 >= cap) {
			out[0] = '\0';
			return -1;
		}
		if (is_escape_char(s)) {
			out[o++] = (char)((s[1] == 0xBF ? 0xC0 : 0x80) |
					  (s[2] & 0x3F));
			s += 3;
		} else {
			out[o++] = (char)*s++;
		}
	}
	out[o] = '\0';
	return (int)o;
}
