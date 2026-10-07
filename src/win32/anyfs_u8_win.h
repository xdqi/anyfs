/*
 * anyfs_u8_win.h — the part of the UTF-8 host layer that needs <windows.h>
 * types. Kept out of anyfs_u8.h, which is force-included into code whose
 * names collide with windows.h macros (QEMU's QAPI, ksmbd-tools' RPC).
 */
#ifndef ANYFS_U8_WIN_H
#define ANYFS_U8_WIN_H

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h> /* before windows.h, which would pull in winsock.h */

#include "anyfs_u8.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CreateFileW on a UTF-8 path (no security attributes, no template). */
HANDLE anyfs_u8_create_file(const char* path, DWORD access, DWORD share,
			    DWORD disposition, DWORD attrs);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* ANYFS_U8_WIN_H */
