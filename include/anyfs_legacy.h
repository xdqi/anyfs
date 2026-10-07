/*
 * anyfs_legacy.h — the legacy file-name encoding of the CLIs.
 *
 * File names that are not UTF-8 were written by systems using a legacy
 * encoding (GBK, Big5, Shift_JIS, ...). The CLIs take
 * --legacy-encoding=<name> (default "auto", from the system locale): it
 * picks the codepage of FAT short names at mount time and decodes such
 * names and labels for display. The UI has the same setting
 * (@anyfs/core names.ts).
 */
#ifndef ANYFS_LEGACY_H
#define ANYFS_LEGACY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	ANYFS_LEGACY_OFF = 0,
	ANYFS_LEGACY_GB18030,
	ANYFS_LEGACY_BIG5,
	ANYFS_LEGACY_SHIFT_JIS,
	ANYFS_LEGACY_EUC_KR,
	ANYFS_LEGACY_WINDOWS_1252,
};

/* "gb18030", "big5", "shift_jis", "euc-kr", "windows-1252", "off", or
 * "auto" (anyfs_legacy_auto). Returns an ANYFS_LEGACY_* value or -1. */
int anyfs_legacy_parse(const char* name);

/* The encoding most likely for this system: the ANSI code page on
 * Windows, the language of LC_ALL / LC_CTYPE / LANG elsewhere. */
int anyfs_legacy_auto(void);

/* The ANYFS_MOUNT_FAT_CP_* enter flag for the encoding's OEM codepage. */
uint32_t anyfs_legacy_fat_flag(int enc);

/* The process-wide encoding used by anyfs_legacy_decode (default: auto). */
void anyfs_legacy_set(int enc);
int anyfs_legacy_get(void);

/* A name or label for display: UTF-8 is copied, anything else is decoded
 * with the process-wide encoding, and bytes that still do not decode show
 * as \xNN. Returns the output length or -1 if it does not fit. */
int anyfs_legacy_decode(const char* in, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ANYFS_LEGACY_H */
