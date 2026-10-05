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
# Compiles for the arch's deployment target in macos_target.sh. ZIG and NM
# (llvm-nm) override the tool lookup. A failed run leaves no archive.
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

# The host sources tools/lkl/lib/Build selects for a POSIX host with
# VIRTIO_NET_FD, minus virtio_net_tap/raw (darwin-netdev-stubs.c), VFIO and
# macvtap (off in the Darwin profile).
SOURCES=(config fs iomem jmp_buf net posix-host utils virtio virtio_blk
         virtio_net virtio_net_fd virtio_net_pipe)
# The Darwin patches those sources need (patches/linux/macho/series).
PATCHES=(08-posix-host-darwin.patch 09-endian-darwin.patch)
# What the archive imports from liblkl-kernel.dylib: EXPORTS in
# build_kernel_dylib.sh, each lkl_X as _lklk_X. Change both together.
KERNEL_EXPORTS=(
    lkl_init lkl_cleanup lkl_syscall lkl_sys_halt lkl_is_running
    lkl_get_free_irq lkl_put_irq lkl_trigger_irq
    lkl_glue_set_host lkl_start_kernel_str
)

die() { echo "build_host_lib: $*" >&2; exit 1; }

# tool VAR CANDIDATE...: print the command in $VAR if set, else the first
# CANDIDATE on PATH; die if there is none. Assign the result to a variable
# (v="$(tool ...)"), so set -e sees a failure.
tool() {
    local var="$1" c
    shift
    local names="$*"
    if [[ -n ${!var:-} ]]; then
        command -v -- "${!var}" || die "$var=${!var} not found"
        return 0
    fi
    for c in "$@"; do
        command -v -- "$c" && return 0
    done
    die "${names// / or } not found on PATH (or set $var)"
}

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

# Fail closed: from here on, a failed run leaves no archive and no objects.
lib="$out/$arch/liblkl-host.a"
dir="$out/$arch/host-obj"
mkdir -p "$out/$arch"
rm -rf "$lib" "$lib.tmp" "$dir"
trap 'rm -f "$lib.tmp"' EXIT

lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
[[ -d "$lkl_out/tools/lkl/include/lkl" ]] || die "no generated headers in $lkl_out: run build_lkl.sh --targets=$target"
# Read the tree, not oot_fs.sh's log: each patch must reverse cleanly.
for p in "${PATCHES[@]}"; do
    patch -p1 -R --dry-run --silent -d "$LINUX_DIR" < "$REPO_DIR/patches/linux/macho/$p" > /dev/null 2>&1 \
        || die "$p is not applied to $LINUX_DIR: run scripts/oot_fs.sh stage --macho"
done
# lkl.h and lkl_config.h include "lkl_autoconf.h" with quotes, which finds one
# next to them before the Darwin profile on the -I path.
[[ ! -e $LINUX_DIR/tools/lkl/include/lkl_autoconf.h ]] \
    || die "$LINUX_DIR/tools/lkl/include/lkl_autoconf.h would shadow the Darwin profile $HERE/autoconf/lkl_autoconf.h: remove it"
# The Darwin profile replaces the generated lkl_autoconf.h, so it must not drop
# what the kernel was configured with: an MMU kernel needs the host's mmap ops.
mmu='^#define LKL_HOST_CONFIG_MMU([[:space:]]|$)'
gen="$lkl_out/tools/lkl/include/lkl_autoconf.h"
if [[ -f $gen ]] && grep -qE "$mmu" "$gen" && ! grep -qE "$mmu" "$HERE/autoconf/lkl_autoconf.h"; then
    die "$gen defines LKL_HOST_CONFIG_MMU but the Darwin profile $HERE/autoconf/lkl_autoconf.h does not"
fi
ZIG="$(tool ZIG zig /opt/zig/zig)"
NM="$(tool NM llvm-nm-19 llvm-nm)"

mkdir -p "$dir"
# -Werror=unguarded-availability: fail on calls to APIs newer than the
# deployment target at compile time. -g and -fno-strict-aliasing as in
# tools/lkl/Makefile.
cflags=(-target "$zt" -O2 -g -Wall -fno-strict-aliasing -Werror=unguarded-availability
        -Werror=deprecated-declarations -D_FILE_OFFSET_BITS=64 -I"$HERE/autoconf"
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
"$ZIG" ar rcs "$lib.tmp" "${objs[@]}"

imports="$("$NM" -u "$lib.tmp" | awk '$NF ~ /^_lklk_/ { print $NF }' | LC_ALL=C sort -u)"
want="$(printf '_lklk_%s\n' "${KERNEL_EXPORTS[@]#lkl_}" | LC_ALL=C sort)"
[[ $imports == "$want" ]] || die "liblkl-host.a imports {${imports//$'\n'/ }} from the kernel," \
    "expected {${want//$'\n'/ }} (EXPORTS in build_kernel_dylib.sh)"
mv "$lib.tmp" "$lib"
echo "build_host_lib: $lib (${#objs[@]} objects)"
