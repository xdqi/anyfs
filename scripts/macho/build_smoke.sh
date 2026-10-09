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
# tests/setup.sh). --image must be an unpartitioned ext4 image: the test
# mounts partition 0, the whole disk. Copy that directory to a Mac and follow
# scripts/macho/smoke/README.md. ZIG overrides the pinned zig. A failed run
# leaves no smoke directory.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
# shellcheck source=macos_target.sh
source "$HERE/macos_target.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"

die() { echo "build_smoke: $*" >&2; exit 1; }

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
macos_zig_target "$arch" > /dev/null

# Fail closed: the bundle is built in smoke.tmp and renamed to smoke only when
# complete, so a failed run leaves neither.
dir="$out/$arch/smoke"
rm -rf "$dir" "$dir.tmp"
trap 'rm -rf "$dir.tmp"' EXIT

lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
[[ -d "$lkl_out/tools/lkl/include/lkl" ]] || die "no generated headers in $lkl_out: run build_lkl.sh --targets=$target"
for f in "$out/$arch/liblkl-kernel.dylib" "$out/$arch/liblkl-host.a" "$image"; do
    [[ -f $f ]] || die "$f not found"
done
if [[ -n ${ZIG:-} ]]; then
    ANYFS_ZIG="$(tool ZIG)"
    export ANYFS_ZIG
fi

mkdir -p "$dir.tmp"
# The flags of build_host_lib.sh: -Werror=unguarded-availability fails on calls
# to APIs newer than the deployment target at compile time.
"$HERE/$arch-macos-cc" -O2 -g -Wall -fno-strict-aliasing -Werror=unguarded-availability \
    -Werror=deprecated-declarations -I"$HERE/autoconf" -I"$LINUX_DIR/tools/lkl/include" \
    -I"$lkl_out/tools/lkl/include" "$HERE/smoke/lkl_macos_smoke.c" \
    "$out/$arch/liblkl-host.a" -L"$out/$arch" -llkl-kernel \
    -Wl,-rpath,@executable_path -o "$dir.tmp/lkl-macos-smoke"
cp "$out/$arch/liblkl-kernel.dylib" "$dir.tmp/"
cp "$image" "$dir.tmp/smoke-ext4.img"
mv "$dir.tmp" "$dir"
echo "build_smoke: $dir"
