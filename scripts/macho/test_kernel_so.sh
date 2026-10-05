#!/bin/bash
# Boot the exact lkl-kernel.so that build_kernel_dylib.sh converts into
# liblkl-kernel.dylib, on Linux, and run the macOS smoke program
# (smoke/lkl_macos_smoke.c) against it: x86_64 natively, arm64 under
# qemu-aarch64 user mode. The program reaches the kernel through
# lkl_macho_shim.c, as on macOS, glue setter included; test_lklk_dlsym.c
# stands in for the dylib's export map and forwards each lklk_X to lkl_X of
# the .so.
#
# Usage: scripts/macho/test_kernel_so.sh [--arch=arm64|x86_64] [--out=DIR] [--image=FILE]
#
#   --arch   test one arch (default: arm64 and x86_64)
#   --out    build output root (default: <repo>/build/macos); the test uses
#            OUT/<arch>/lkl-kernel.so
#   --image  unpartitioned ext4 image (default: <repo>/tests/images/ext4.img,
#            made by tests/setup.sh); each run gets a fresh scratch copy
#
# Needs, for each arch tested:
#   - OUT/<arch>/lkl-kernel.so: run build_kernel_dylib.sh --arch=<arch> first;
#   - the LKL build tree <repo>/lkl-linux-arm64 or <repo>/lkl-linux-amd64
#     (build_lkl.sh --targets=linux-arm64|linux-amd64): its
#     tools/lkl/lib/liblkl-in.o is the ELF host library and its
#     tools/lkl/include holds the generated headers;
#   - x86_64: an x86_64 host and gcc;
#   - arm64: aarch64-linux-gnu-gcc, and qemu-aarch64-static or qemu-aarch64
#     with the aarch64 libc in /usr/aarch64-linux-gnu. Without these, arm64
#     prints SKIP instead.
# A missing build output is a FAIL, and so is a run in which no arch was
# tested. The scratch directory is always removed.
#
# Checks per arch: the program exits 0 and prints "PASS (N checks)" after N
# "ok" lines, and the kernel's "Memory:" boot line reports rwdata, rodata and
# init below 1 GiB each. A link that moves lkl.o's sections out of input order
# still boots and passes the program, but reports absurd sizes there (the
# spec's "Kernel link").
#
# The host library here is the stock Linux one: the Darwin host paths of
# patch 08 (the semaphore fallback, the timer emulation, thread_stack(), the
# preadv/pwritev fallback) are exercised only on a Mac (smoke/README.md).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$HERE/../.." && pwd)"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"

LIMIT_K=$((1024 * 1024))  # 1 GiB, in the K units of the Memory: line

die() { echo "test_kernel_so: $*" >&2; exit 1; }

arches=() out="$REPO_DIR/build/macos" image="$REPO_DIR/tests/images/ext4.img"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=arm64|--arch=x86_64) arches=("${1#--arch=}") ;;
        --out=*)   out="${1#--out=}" ;;
        --image=*) image="${1#--image=}" ;;
        -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)         die "unknown argument: $1" ;;
    esac
    shift
done
[[ ${#arches[@]} -gt 0 ]] || arches=(arm64 x86_64)
[[ -f $image ]] || die "$image not found: run tests/setup.sh or pass --image"

tmp="$(mktemp -d -p /var/tmp test_kernel_so.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

# test_arch ARCH: build and run the smoke program for ARCH against
# OUT/ARCH/lkl-kernel.so. Prints one PASS, FAIL or SKIP line; returns 0 on
# PASS, 1 on FAIL, 2 on SKIP. It is called with ||, so set -e is off in here:
# every step checks its own status.
test_arch() {
    local arch="$1" target cc run=() qemu so lkl_out host dir log rc n oks mem what v
    case "$arch" in
        x86_64)
            target=linux-amd64 cc=gcc
            if [[ $(uname -m) != x86_64 ]]; then
                echo "SKIP $arch: needs an x86_64 host"; return 2
            fi
            ;;
        arm64)
            target=linux-arm64 cc=aarch64-linux-gnu-gcc
            qemu="$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)"
            if [[ -z $qemu ]] || ! command -v "$cc" > /dev/null; then
                echo "SKIP $arch: needs aarch64-linux-gnu-gcc and qemu-aarch64(-static)"; return 2
            fi
            if [[ ! -d /usr/aarch64-linux-gnu/lib ]]; then
                echo "SKIP $arch: needs the aarch64 libc in /usr/aarch64-linux-gnu"; return 2
            fi
            run=("$qemu" -L /usr/aarch64-linux-gnu)
            ;;
    esac
    command -v "$cc" > /dev/null || { echo "FAIL $arch: $cc not found"; return 1; }
    so="$out/$arch/lkl-kernel.so"
    lkl_out="$REPO_DIR/lkl-$target"
    host="$lkl_out/tools/lkl/lib/liblkl-in.o"
    if [[ ! -f $so ]]; then
        echo "FAIL $arch: $so not found: run build_kernel_dylib.sh --arch=$arch"; return 1
    fi
    if [[ ! -f $host || ! -d $lkl_out/tools/lkl/include/lkl ]]; then
        echo "FAIL $arch: no liblkl-in.o or generated headers in $lkl_out:" \
            "run build_lkl.sh --targets=$target"
        return 1
    fi

    dir="$tmp/$arch" log="$tmp/$arch/smoke.log"
    mkdir -p "$dir" || { echo "FAIL $arch: mkdir $dir"; return 1; }
    if ! "$cc" -O2 -Wall -Wextra -Werror -isystem "$LINUX_DIR/tools/lkl/include" \
            -isystem "$lkl_out/tools/lkl/include" -o "$dir/lkl-smoke" \
            "$HERE/smoke/lkl_macos_smoke.c" "$HERE/lkl_macho_shim.c" "$HERE/test_lklk_dlsym.c" \
            "$host" -pthread -ldl -lrt > "$dir/cc.log" 2>&1; then
        echo "FAIL $arch: building the smoke program failed:"; sed 's/^/    /' "$dir/cc.log"
        return 1
    fi
    cp "$image" "$dir/ext4.img" || { echo "FAIL $arch: copying $image failed"; return 1; }

    # The program cuts off a hang after 120 s itself; timeout is the backstop.
    rc=0
    LKLK_KERNEL_SO="$so" timeout -k 10 300 "${run[@]}" "$dir/lkl-smoke" "$dir/ext4.img" \
        > "$log" 2>&1 || rc=$?

    local errors=()
    [[ $rc -eq 0 ]] || errors+=("exit status $rc")
    n="$(sed -nE 's/^PASS \(([0-9]+) checks\)$/\1/p' "$log")"
    oks="$(grep -c '^ok   ' "$log")"
    if [[ -z $n ]]; then
        errors+=("no PASS line")
    elif [[ $n -ne $oks || $n -eq 0 ]]; then
        errors+=("PASS ($n checks) after $oks ok lines")
    fi
    mem="$(grep -m 1 -oE 'Memory: .*' "$log")"
    if [[ -z $mem ]]; then
        errors+=("no Memory: line")
    else
        for what in rwdata rodata init; do
            v="$(grep -oE "[0-9]+K $what" <<< "$mem" | grep -oE '^[0-9]+')"
            if [[ -z $v ]]; then
                errors+=("no $what size in the Memory: line")
            elif [[ ${#v} -gt 15 ]] || (( v >= LIMIT_K )); then
                errors+=("Memory: ${v}K $what is not below 1 GiB: a bad link")
            fi
        done
    fi
    if [[ ${#errors[@]} -gt 0 ]]; then
        echo "FAIL $arch: $(printf '%s; ' "${errors[@]}" | sed 's/; $//')"
        echo "  $so"
        [[ -z $mem ]] || echo "  $mem"
        echo "  output:"; sed 's/^/    /' "$log"
        return 1
    fi
    echo "PASS $arch: $n checks; $mem"
    echo "  $so"
}

failures=0 tested=0
for arch in "${arches[@]}"; do
    rc=0
    test_arch "$arch" || rc=$?
    case $rc in
        0) tested=$((tested + 1)) ;;
        1) tested=$((tested + 1)) failures=$((failures + 1)) ;;
    esac
done
if [[ $tested -eq 0 ]]; then
    echo "FAILED test_kernel_so: no arch was tested"; exit 1
elif [[ $failures -gt 0 ]]; then
    echo "FAILED test_kernel_so: $failures of $tested"; exit 1
fi
echo "PASS test_kernel_so"
