/*
 * anyfs_legacy.c — the CLIs' legacy file-name encoding; see anyfs_legacy.h.
 *
 * Decoding uses the host: MultiByteToWideChar on Windows, iconv elsewhere
 * (glibc loads its converters from the system's gconv modules; the shipped
 * Linux binaries link glibc dynamically). Under wasm, where nothing prints
 * labels, names that are not UTF-8 always show as \xNN.
 */
#include "anyfs_legacy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#elif !defined(__EMSCRIPTEN__)
#include <iconv.h>
#endif

/* ANYFS_MOUNT_FAT_CP_* (include/anyfs.h), restated so this unit builds
 * without LKL headers. */
#define FAT_CP_SHIFT 8

static const struct {
	const char* name;
	const char* iconv;
	unsigned win_cp;
	unsigned win_cp_fallback; /* when win_cp is not installed (wine: 54936) */
	uint32_t fat_cp;	  /* index in the ANYFS_MOUNT_FAT_CP_* field */
} encodings[] = {
    [ANYFS_LEGACY_OFF] = {"off", NULL, 0, 0, 0},
    [ANYFS_LEGACY_GB18030] = {"gb18030", "GB18030", 54936, 936, 1},
    [ANYFS_LEGACY_BIG5] = {"big5", "BIG5", 950, 0, 2},
    [ANYFS_LEGACY_SHIFT_JIS] = {"shift_jis", "SHIFT_JIS", 932, 0, 3},
    [ANYFS_LEGACY_EUC_KR] = {"euc-kr", "EUC-KR", 949, 0, 4},
    [ANYFS_LEGACY_WINDOWS_1252] = {"windows-1252", "CP1252", 1252, 0, 0},
};
#define N_ENCODINGS (int)(sizeof(encodings) / sizeof(encodings[0]))

int anyfs_legacy_parse(const char* name)
{
	if (!name)
		return -1;
	if (strcmp(name, "auto") == 0)
		return anyfs_legacy_auto();
	for (int i = 0; i < N_ENCODINGS; i++)
		if (strcmp(name, encodings[i].name) == 0)
			return i;
	return -1;
}

int anyfs_legacy_auto(void)
{
#ifdef _WIN32
	switch (GetACP()) {
	case 936:
	case 54936:
		return ANYFS_LEGACY_GB18030;
	case 950:
		return ANYFS_LEGACY_BIG5;
	case 932:
		return ANYFS_LEGACY_SHIFT_JIS;
	case 949:
		return ANYFS_LEGACY_EUC_KR;
	default:
		return ANYFS_LEGACY_WINDOWS_1252;
	}
#else
	const char* v = NULL;
	static const char* const vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
	for (int i = 0; i < 3 && !(v && *v); i++)
		v = getenv(vars[i]);
	if (!v)
		v = "";
	/* zh_TW, zh_HK, zh_MO and Hant: Big5; any other zh: GB18030. */
	if (strncmp(v, "zh", 2) == 0)
		return strstr(v, "_TW") || strstr(v, "_HK") ||
			       strstr(v, "_MO") || strstr(v, "Hant")
			   ? ANYFS_LEGACY_BIG5
			   : ANYFS_LEGACY_GB18030;
	if (strncmp(v, "ja", 2) == 0)
		return ANYFS_LEGACY_SHIFT_JIS;
	if (strncmp(v, "ko", 2) == 0)
		return ANYFS_LEGACY_EUC_KR;
	return ANYFS_LEGACY_WINDOWS_1252;
#endif
}

uint32_t anyfs_legacy_fat_flag(int enc)
{
	if (enc < 0 || enc >= N_ENCODINGS)
		return 0;
	return encodings[enc].fat_cp << FAT_CP_SHIFT;
}

static int g_enc = -1; /* -1: not set yet, resolve with anyfs_legacy_auto */

void anyfs_legacy_set(int enc)
{
	g_enc = enc >= 0 && enc < N_ENCODINGS ? enc : ANYFS_LEGACY_OFF;
}

int anyfs_legacy_get(void)
{
	if (g_enc < 0)
		g_enc = anyfs_legacy_auto();
	return g_enc;
}

/* Strict UTF-8 check (the rules of anyfs_name.c). */
static int valid_utf8(const unsigned char* s)
{
	while (*s) {
		unsigned c = *s;
		size_t n = c < 0x80		      ? 1
			   : (c >= 0xC2 && c <= 0xDF) ? 2
			   : (c >= 0xE0 && c <= 0xEF) ? 3
			   : (c >= 0xF0 && c <= 0xF4) ? 4
						      : 0;
		if (!n)
			return 0;
		if (n >= 3) {
			unsigned lo = c == 0xE0	  ? 0xA0
				      : c == 0xF0 ? 0x90
						  : 0x80;
			unsigned hi = c == 0xED	  ? 0x9F
				      : c == 0xF4 ? 0x8F
						  : 0xBF;
			if (s[1] < lo || s[1] > hi)
				return 0;
		}
		for (size_t k = 1; k < n; k++)
			if ((s[k] & 0xC0) != 0x80)
				return 0;
		s += n;
	}
	return 1;
}

/* Decode with the host converter. 0 on success, -1 if it cannot. */
static int host_decode(int enc, const char* in, char* out, size_t cap)
{
#ifdef _WIN32
	unsigned cp = encodings[enc].win_cp;
	int wn = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, in, -1, NULL, 0);
	if (wn <= 0 && GetLastError() == ERROR_INVALID_PARAMETER &&
	    encodings[enc].win_cp_fallback) {
		cp = encodings[enc].win_cp_fallback;
		wn = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, in, -1, NULL,
					 0);
	}
	if (wn <= 0)
		return -1;
	wchar_t* w = malloc((size_t)wn * sizeof(wchar_t));
	if (!w)
		return -1;
	MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, in, -1, w, wn);
	int n =
	    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
	free(w);
	return n > 0 ? 0 : -1;
#elif !defined(__EMSCRIPTEN__)
	iconv_t cd = iconv_open("UTF-8", encodings[enc].iconv);
	if (cd == (iconv_t)-1)
		return -1;
	char* ip = (char*)in;
	size_t il = strlen(in);
	char* op = out;
	size_t ol = cap - 1;
	size_t r = iconv(cd, &ip, &il, &op, &ol);
	if (r != (size_t)-1)
		r = iconv(cd, NULL, NULL, &op, &ol); /* flush the shift state */
	iconv_close(cd);
	if (r == (size_t)-1 || il != 0)
		return -1;
	*op = '\0';
	return 0;
#else
	(void)enc;
	(void)in;
	(void)out;
	(void)cap;
	return -1;
#endif
}

int anyfs_legacy_decode(const char* in, char* out, size_t cap)
{
	const unsigned char* s = (const unsigned char*)in;
	int enc = anyfs_legacy_get();

	if (!out || cap == 0)
		return -1;
	if (valid_utf8(s) ||
	    (enc != ANYFS_LEGACY_OFF && host_decode(enc, in, out, cap) == 0)) {
		if (valid_utf8(s)) {
			size_t n = strlen(in);
			if (n >= cap)
				goto overflow;
			memcpy(out, in, n + 1);
		}
		return (int)strlen(out);
	}
	/* \xNN for every byte that is not part of valid UTF-8. */
	size_t o = 0;
	for (; *s; s++) {
		unsigned char one[2] = {*s, 0};
		int keep = *s < 0x80 || valid_utf8(one);
		if (o + (keep ? 1 : 4) >= cap)
			goto overflow;
		if (keep)
			out[o++] = (char)*s;
		else
			o += (size_t)snprintf(out + o, cap - o, "\\x%02X", *s);
	}
	out[o] = '\0';
	return (int)o;

overflow:
	out[0] = '\0';
	return -1;
}
