#!/bin/bash
# Build liblkl-host.a, the LKL host library for macOS, on Linux with zig cc.
#
# Usage: build_host_lib.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR]
#
#   --lkl-out  LKL build tree whose tools/lkl/include holds the generated lkl/
#              headers (default: <repo>/lkl-linux-arm64 or <repo>/lkl-linux-amd64)
#   --out      output root (default: <repo>/build/macos); writes
#              OUT/<arch>/liblkl-host.a
#
# Needs the Darwin host patches in $LINUX_DIR (scripts/oot_fs.sh stage --macho).
# Compiles for the arch's deployment target in macos_target.sh.
# The kernel is liblkl-kernel.dylib (build_kernel_dylib.sh), reached through
# lkl_macho_shim.c.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
# shellcheck source=macos_target.sh
source "$HERE/macos_target.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OOT_DIR="${OOT_DIR:-$HOME/oot-fs}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"

# The host sources tools/lkl/lib/Build selects for a POSIX host with
# VIRTIO_NET_FD, minus virtio_net_tap/raw (darwin-netdev-stubs.c), VFIO and
# macvtap (off in the Darwin profile).
SOURCES=(config fs iomem jmp_buf net posix-host utils virtio virtio_blk
         virtio_net virtio_net_fd virtio_net_pipe)

die() { echo "build_host_lib: $*" >&2; exit 1; }

arch="" lkl_out="" out="$REPO_DIR/build/macos"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)    arch="${1#--arch=}" ;;
        --lkl-out=*) lkl_out="${1#--lkl-out=}" ;;
        --out=*)     out="${1#--out=}" ;;
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
[[ -d "$lkl_out/tools/lkl/include/lkl" ]] || die "no generated headers in $lkl_out: run build_lkl.sh --targets=$target"
for p in 08-posix-host-darwin.patch 09-endian-darwin.patch; do
    grep -qxF "$p" "$OOT_DIR/.applied.macho" 2>/dev/null \
        || die "$p is not applied to $LINUX_DIR: run scripts/oot_fs.sh stage --macho"
done

dir="$out/$arch/host-obj"
mkdir -p "$dir"
# Fail on calls to APIs newer than the deployment target at compile time.
cflags=(-target "$zt" -O2 -Wall -Werror=unguarded-availability -D_FILE_OFFSET_BITS=64 -I"$HERE/autoconf"
        -I"$LINUX_DIR/tools/lkl/include" -I"$lkl_out/tools/lkl/include")
objs=()
for s in "${SOURCES[@]}"; do
    "$ZIG" cc "${cflags[@]}" -c "$LINUX_DIR/tools/lkl/lib/$s.c" -o "$dir/$s.o"
    objs+=("$dir/$s.o")
done
for s in darwin-netdev-stubs lkl_macho_shim; do
    "$ZIG" cc "${cflags[@]}" -c "$HERE/$s.c" -o "$dir/$s.o"
    objs+=("$dir/$s.o")
done
rm -f "$out/$arch/liblkl-host.a"
"$ZIG" ar rcs "$out/$arch/liblkl-host.a" "${objs[@]}"
echo "build_host_lib: $out/$arch/liblkl-host.a (${#objs[@]} objects)"
