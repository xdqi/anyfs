#!/usr/bin/env bash
# Assemble the native payload of a packaged app into one directory:
#
#   <dest>/anyfs_native.node     the N-API addon (required)
#   <dest>/drivelist.node        drive enumeration (optional; sibling
#                                drivelist-anyfs checkout, not built in CI)
#   <dest>/*.dll                 win32: the addon's runtime DLL closure
#
# package.sh copies the directory as-is into resources/native/. CI uploads
# it as the anyfs-native-<platform>-<arch> artifact, so the packaging job
# never needs the LKL/QEMU build trees.
#
# Everything is stripped of debug info here, where the matching toolchain is
# available (liblkl.dll: ~220 MiB -> ~15 MiB).
#
# Usage: collect-native.sh <linux|win32> <dest-dir>
# Inputs (environment, defaults match the local build layout):
#   ANYFS_NATIVE_NODE  addon to ship
#   DRIVELIST_NODE     drivelist addon; shipped when the file exists
#   win32 only: LKL_MINGW64, QEMU_BLD_MINGW64, MINGW_SYSROOT (DLL search dirs)
set -euo pipefail

platform="${1:?usage: collect-native.sh <linux|win32> <dest-dir>}"
dest="${2:?usage: collect-native.sh <linux|win32> <dest-dir>}"

script_dir="$(cd "$(dirname "$0")" && pwd)"
ts_root="$(cd "$script_dir/../../.." && pwd)"
repo_root="$(cd "$ts_root/.." && pwd)"

rm -rf "$dest"
mkdir -p "$dest"

case "$platform" in
linux)
    node="${ANYFS_NATIVE_NODE:-$ts_root/packages/anyfs-native/build/Release/anyfs_native.node}"
    drivelist="${DRIVELIST_NODE:-$repo_root/../drivelist-anyfs/build/Release/drivelist.node}"
    strip_cmd=(strip --strip-debug)
    ;;
win32)
    node="${ANYFS_NATIVE_NODE:-$ts_root/packages/anyfs-native/build-win64/anyfs_native.node}"
    drivelist="${DRIVELIST_NODE:-$repo_root/../drivelist-anyfs/build-win64/drivelist.node}"
    strip_cmd=("${STRIP:-x86_64-w64-mingw32-strip}" --strip-debug)
    ;;
*)
    echo "collect-native: unsupported platform '$platform' (linux|win32)" >&2
    exit 2
    ;;
esac

[[ -f "$node" ]] || { echo "collect-native: missing $node" >&2; exit 1; }
cp -- "$node" "$dest/anyfs_native.node"
roots=("$dest/anyfs_native.node")
if [[ -f "$drivelist" ]]; then
    cp -- "$drivelist" "$dest/drivelist.node"
    roots+=("$dest/drivelist.node")
else
    echo "collect-native: no drivelist.node at $drivelist; the drives panel will be unavailable"
fi

if [[ "$platform" == win32 ]]; then
    mingw="${MINGW_SYSROOT:-/opt/msys2-cross/mingw64}"
    bash "$script_dir/collect-win64-dlls.sh" "$dest" "${roots[@]}" -- \
        "${LKL_MINGW64:-$repo_root/lkl-mingw64}/tools/lkl/lib" \
        "${QEMU_BLD_MINGW64:-$HOME/qemu/build-anyfs-mingw64}" \
        "$mingw/bin"
fi

for f in "$dest"/*; do
    case "$f" in *.node|*.dll) "${strip_cmd[@]}" "$f" ;; esac
done

echo "collect-native: $platform payload in $dest"
ls -l "$dest"
