#!/usr/bin/env bash
# Run the mingw64 binaries under wine with non-ASCII paths: the images in a
# directory named "测试 café", and %TEMP% pointing at another one (the probe
# spool and the snapshot overlay go there).
#   - anyfs-lspart.exe: the image opens from the command line, the
#     filesystems are typed (the spool worked), output through a pipe is
#     valid UTF-8;
#   - test_open_paths.exe: raw backend, QEMU backend and snapshot mode open
#     the image, mount its FAT partition and read a file (the core code the
#     Electron addon links);
#   - scripts/check_win_imports.sh: no ANSI file/env calls in anyfs code.
#
# Usage: tests/wine/u8-cli.sh [build-dir]   (default build-anyfs-mingw64)
# Needs wine, python3, mkfs.ext4; qemu-img for the qcow2/vmdk cases.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$repo/build-anyfs-mingw64}"
lspart="$build/src/lspart/anyfs-lspart.exe"
[[ -x "$lspart" ]] || { echo "missing $lspart" >&2; exit 2; }

work="$(mktemp -d "${HOME}/.cache/anyfs-u8-cli.XXXXXX")"
# shellcheck disable=SC2317  # called through trap
cleanup() {
    wineserver -k 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
export WINEPREFIX="$work/prefix" WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all
wineboot -i >/dev/null 2>&1
WINEPATH="$(winepath -w "$HOME/qemu/build-anyfs-mingw64")"
WINEPATH+=";$(winepath -w "$repo/lkl-mingw64/tools/lkl/lib")"
WINEPATH+=";$(winepath -w /opt/msys2-cross/mingw64/bin)"
export WINEPATH

dir="$work/测试 café"
tmp="$work/临时 temp"
mkdir -p "$dir" "$tmp"
python3 "$repo/tests/make_names_image.py" "$dir/names.img"
images=("$dir/names.img")
if command -v qemu-img >/dev/null; then
    qemu-img convert -O qcow2 "$dir/names.img" "$dir/名字.qcow2"
    qemu-img convert -O vmdk "$dir/names.img" "$dir/名字.vmdk"
    images+=("$dir/名字.qcow2" "$dir/名字.vmdk")
fi
wtmp="$(winepath -w "$tmp")"

fail=0
for img in "${images[@]}"; do
    out="$work/out.txt"
    rc=0
    TEMP="$wtmp" TMP="$wtmp" \
        wine "$lspart" "$(winepath -w "$img")" >"$out" 2>"$work/err.txt" || rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "FAIL lspart $(basename "$img"): exit $rc"; cat "$work/err.txt"; fail=1; continue
    fi
    if ! iconv -f UTF-8 -t UTF-8 "$out" >/dev/null 2>&1; then
        echo "FAIL lspart $(basename "$img"): output is not UTF-8"; fail=1; continue
    fi
    if ! grep -q 'disk0/p1 .* vfat' "$out" || ! grep -q 'disk0/p2 .* ext4' "$out"; then
        echo "FAIL lspart $(basename "$img"): partitions not typed"; cat "$out"; fail=1; continue
    fi
    echo "ok   lspart $(basename "$img")"
done

# open flags: 1 READONLY, 2 BACKEND_RAW, 8 BACKEND_QEMU, 16 SNAPSHOT
probe="$build/test_open_paths.exe"
if [[ -x "$probe" ]]; then
    args=(3 "$(winepath -w "$dir/names.img")" 9 "$(winepath -w "$dir/names.img")")
    if [[ -f "$dir/名字.qcow2" ]]; then
        args+=(1 "$(winepath -w "$dir/名字.qcow2")" 1 "$(winepath -w "$dir/名字.vmdk")")
        args+=(24 "$(winepath -w "$dir/名字.qcow2")")
    fi
    rc=0
    TEMP="$wtmp" TMP="$wtmp" timeout 300 \
        wine "$probe" "${args[@]}" >"$work/probe.txt" 2>/dev/null || rc=$?
    grep -E '^(ok|FAIL)' "$work/probe.txt" | sed "s|^\(ok  \|FAIL\) .*\\\\|\1 open_paths |" || true
    [[ $rc -eq 0 ]] || { echo "FAIL open_paths: exit $rc"; fail=1; }
else
    echo "skip open_paths (not built)"
fi

if "$repo/scripts/check_win_imports.sh" "$build" >"$work/imports.txt"; then
    echo "ok   import gate"
else
    grep -v '^ok' "$work/imports.txt"
    fail=1
fi
exit "$fail"
