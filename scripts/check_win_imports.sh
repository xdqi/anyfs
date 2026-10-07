#!/usr/bin/env bash
# check_win_imports.sh — fail when anyfs's own Windows code calls an ANSI
# file, path or environment function.
#
# anyfs keeps every string in UTF-8 and crosses the Windows boundary through
# the W (UTF-16) APIs (src/win32/anyfs_u8.h). An ANSI call such as
# CreateFileA or msvcrt's fopen reads its char* argument in the system code
# page, so a path outside that code page breaks.
#
# The gate reads the undefined symbols of anyfs's own objects (the core
# archive, the u8 layer, ksmbd-tools as built into anyfs-ksmbd, every CLI's
# objects, and the addon objects if given), not the PE import tables: those
# also carry the ANSI calls of statically linked libblkid and of the mingw
# CRT helpers it pulls in (dirent, the stat fallback), which anyfs never
# hands a path (it gives libblkid an fd), and they would hide a new ANSI call
# in anyfs behind an import that is already there.
#
# Usage: scripts/check_win_imports.sh [build-dir] [extra .o/.a ...]
#        (default build-dir: build-anyfs-mingw64)
# Env:   NM (default: x86_64-w64-mingw32-nm, then the msys2-cross one)
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$repo/build-anyfs-mingw64}"
shift || true
NM="${NM:-$(command -v x86_64-w64-mingw32-nm ||
    echo /opt/msys2-cross/bin/x86_64-w64-mingw32-nm)}"

ANSI_RE='^(CreateFileA|CreateFile2A|FindFirstFileA|FindFirstFileExA|GetTempPathA|GetTempFileNameA|GetModuleFileNameA|DeleteFileA|MoveFileA|MoveFileExA|CopyFileA|CreateDirectoryA|RemoveDirectoryA|GetFileAttributesA|GetFileAttributesExA|SetFileAttributesA|GetFullPathNameA|GetLongPathNameA|GetShortPathNameA|GetEnvironmentVariableA|SetEnvironmentVariableA|CreateProcessA|fopen|fopen64|freopen|_open|open|_sopen|_stat|_stat32|_stat64|_stati64|_stat64i32|stat|stat64|_access|access|__mingw_access|_unlink|unlink|getenv|_mktemp|mkstemp|_tempnam|tmpnam|_fullpath|realpath|_mkdir|mkdir|_rmdir|rmdir|remove|rename|opendir|_findfirst|_findfirst64|_findfirst64i32|_chdir|chdir|_getcwd|getcwd)$'

shopt -s nullglob
objs=(
    "$build/libanyfs_core.a"
    "$build/libanyfs_u8.a"
    "$build/libanyfs_u8_main.a"
    "$build/libksmbd_tools_lib.a"
    "$build/libhost_proxy.a"
    "$build"/anyfs-*.exe.p/*.obj
    "$build"/src/lspart/anyfs-lspart.exe.p/*.obj
    "$@"
)
[[ -f "$build/libanyfs_core.a" ]] || { echo "missing $build/libanyfs_core.a" >&2; exit 2; }

fail=0
for o in "${objs[@]}"; do
    [[ -f "$o" ]] || continue
    bad=$("$NM" -u "$o" 2>/dev/null | awk '
        /:$/ { member = $0; sub(/:$/, "", member); next }
        $1 == "U" { sym = $2; sub(/^__imp_/, "", sym); print (member == "" ? "-" : member), sym }' |
        while read -r member sym; do
            if [[ "$sym" =~ $ANSI_RE ]]; then echo "$member $sym"; fi
        done | sort -u)
    if [[ -n "$bad" ]]; then
        echo "FAIL ${o#"$build"/}:"
        while IFS= read -r l; do echo "    $l"; done <<<"$bad"
        fail=1
    else
        echo "ok   ${o#"$build"/}"
    fi
done
exit "$fail"
