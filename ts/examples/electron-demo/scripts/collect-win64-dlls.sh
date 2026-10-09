#!/usr/bin/env bash
# Copy the runtime DLL closure of one or more PE files into a directory.
#
# Walks the import tables recursively (objdump -p "DLL Name:"), resolving each
# DLL against the given search directories, first match wins. A DLL found in no
# search directory must be a Windows system DLL (allowlist below), otherwise the
# script fails: a missing transitive DLL is exactly what makes Windows report
# err=126 and the app fall back to wasm (F12).
#
# Usage: collect-win64-dlls.sh <dest-dir> <pe-file>... -- <search-dir>...
set -euo pipefail

usage() { echo "usage: collect-win64-dlls.sh <dest-dir> <pe-file>... -- <search-dir>..." >&2; exit 2; }
[[ $# -ge 4 ]] || usage
dest="$1"; shift
roots=()
while [[ $# -gt 0 && "$1" != -- ]]; do roots+=("$1"); shift; done
[[ "${1:-}" == -- ]] || usage
shift
search=("$@")
[[ ${#roots[@]} -gt 0 && ${#search[@]} -gt 0 ]] || usage

OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
command -v "$OBJDUMP" >/dev/null || { echo "collect-win64-dlls: $OBJDUMP not found" >&2; exit 1; }

# Shipped with Windows 10+ (or the host exe for node.exe, see binding.cc's
# delay-load hook). Lower-case; api-ms-win-* / ext-ms-* are API sets.
is_system_dll() {
    case "$1" in
        api-ms-win-*|ext-ms-*) return 0 ;;
        advapi32.dll|bcrypt.dll|bcryptprimitives.dll|cfgmgr32.dll|comctl32.dll|\
        comdlg32.dll|crypt32.dll|dbghelp.dll|dnsapi.dll|dwmapi.dll|gdi32.dll|\
        imm32.dll|iphlpapi.dll|kernel32.dll|kernelbase.dll|msvcrt.dll|ncrypt.dll|\
        netapi32.dll|node.exe|normaliz.dll|ntdll.dll|ole32.dll|oleaut32.dll|\
        powrprof.dll|psapi.dll|rpcrt4.dll|secur32.dll|setupapi.dll|shell32.dll|\
        shlwapi.dll|ucrtbase.dll|user32.dll|userenv.dll|uxtheme.dll|version.dll|\
        winhttp.dll|wininet.dll|winmm.dll|wldap32.dll|ws2_32.dll|wsock32.dll)
            return 0 ;;
    esac
    return 1
}

imports_of() {
    "$OBJDUMP" -p "$1" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' | tr -d '\r'
}

find_dll() {
    local name="$1" dir f
    for dir in "${search[@]}"; do
        # Case-insensitive: import tables say KERNEL32.dll, files say kernel32.dll.
        f="$(find -L "$dir" -maxdepth 1 -type f -iname "$name" -print -quit 2>/dev/null)"
        [[ -n "$f" ]] && { echo "$f"; return 0; }
    done
    return 1
}

mkdir -p "$dest"
declare -A seen=()
queue=("${roots[@]}")
missing=0
while [[ ${#queue[@]} -gt 0 ]]; do
    pe="${queue[0]}"
    queue=("${queue[@]:1}")
    while IFS= read -r dll; do
        [[ -n "$dll" ]] || continue
        key="${dll,,}"
        [[ -z "${seen[$key]:-}" ]] || continue
        seen[$key]=1
        if path="$(find_dll "$dll")"; then
            cp -L -- "$path" "$dest/$(basename "$path")"
            echo "  $(basename "$path")  <- $path"
            queue+=("$path")
        elif is_system_dll "$key"; then
            :
        else
            echo "collect-win64-dlls: $dll (needed by $(basename "$pe")) not found in: ${search[*]}" >&2
            missing=1
        fi
    done < <(imports_of "$pe")
done
[[ $missing -eq 0 ]] || exit 1
echo "collect-win64-dlls: $(find "$dest" -maxdepth 1 -iname '*.dll' | wc -l) DLLs in $dest"
