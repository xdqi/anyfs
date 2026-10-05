#!/bin/bash
# Cross-build the macOS smoke test bundle on Linux.
#
# Usage: build_smoke.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR] [--image=FILE]
#
# Needs OUT/<arch>/liblkl-kernel.dylib (build_kernel_dylib.sh) and
# OUT/<arch>/liblkl-host.a (build_host_lib.sh). Writes OUT/<arch>/smoke/ with
# lkl-macos-smoke, built for the arch's deployment target in macos_target.sh,
# liblkl-kernel.dylib (found through @executable_path) and smoke-ext4.img, a
# copy of --image (default: <repo>/tests/images/ext4.img, made by
# tests/setup.sh). Copy that directory to a Mac and follow
# scripts/macho/smoke/README.md.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
# shellcheck source=macos_target.sh
source "$HERE/macos_target.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"

die() { echo "build_smoke: $*" >&2; exit 1; }

arch="" lkl_out="" out="$REPO_DIR/build/macos" image="$REPO_DIR/tests/images/ext4.img"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)    arch="${1#--arch=}" ;;
        --lkl-out=*) lkl_out="${1#--lkl-out=}" ;;
        --out=*)     out="${1#--out=}" ;;
        --image=*)   image="${1#--image=}" ;;
        -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
    shift
done

case "$arch" in
    arm64)  target=linux-arm64 ;;
    x86_64) target=linux-amd64 ;;
    *)      die "--arch=arm64|x86_64 is required" ;;
esac
zt="$(macos_zig_target "$arch")"
lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
for f in "$out/$arch/liblkl-kernel.dylib" "$out/$arch/liblkl-host.a" "$image"; do
    [[ -f $f ]] || die "$f not found"
done

dir="$out/$arch/smoke"
rm -rf "$dir"
mkdir -p "$dir"
# Fail on calls to APIs newer than the deployment target at compile time.
"$ZIG" cc -target "$zt" -O2 -Wall -Werror=unguarded-availability -I"$HERE/autoconf" -I"$LINUX_DIR/tools/lkl/include" \
    -I"$lkl_out/tools/lkl/include" "$HERE/smoke/lkl_macos_smoke.c" \
    "$out/$arch/liblkl-host.a" -L"$out/$arch" -llkl-kernel \
    -Wl,-rpath,@executable_path -o "$dir/lkl-macos-smoke"
cp "$out/$arch/liblkl-kernel.dylib" "$dir/"
cp "$image" "$dir/smoke-ext4.img"
echo "build_smoke: $dir"
