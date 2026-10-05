#!/bin/bash
# Native (Linux) unit tests for the two sides of the ELF/Mach-O boundary:
# lkl_elf_glue.c and lkl_macho_shim.c. Neither is Darwin-specific C, so both
# build with the host gcc. Any compiler warning fails the test.
#
# Usage: scripts/macho/test_glue.sh [LKL_OUT]
#   LKL_OUT  LKL build tree with the generated tools/lkl/include headers the
#            shim needs (default: <repo>/lkl-linux-amd64)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$HERE/../.." && pwd)"
# shellcheck source=../lib/config.sh
source "$REPO_DIR/scripts/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
lkl_out="${1:-$REPO_DIR/lkl-linux-amd64}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
warn=(-Wall -Wextra -Werror)

# freestanding CC [CFLAGS...]: build the glue the way build_kernel_dylib.sh
# does. Inside the kernel image it may import nothing but lkl_start_kernel:
# no libc, and no memcpy/memset the compiler made up.
freestanding() {
    local cc="$1" nm undefined
    shift
    nm="${cc%gcc}nm"
    "$cc" -O2 "${warn[@]}" -fPIC -ffreestanding -fno-builtin -fno-stack-protector "$@" \
        -c "$HERE/lkl_elf_glue.c" -o "$tmp/glue.o"
    undefined="$("$nm" -u "$tmp/glue.o" | awk '{ print $NF }' | tr '\n' ' ')"
    if [[ $undefined != "lkl_start_kernel " ]]; then
        echo "FAIL $cc: lkl_elf_glue.o imports '$undefined', expected only lkl_start_kernel"
        exit 1
    fi
    echo "PASS freestanding lkl_elf_glue.o: $cc${*:+ $*} imports only lkl_start_kernel"
}
freestanding gcc
if command -v aarch64-linux-gnu-gcc > /dev/null; then
    freestanding aarch64-linux-gnu-gcc -ffixed-x18 -mno-outline-atomics
fi

gcc -O2 "${warn[@]}" -o "$tmp/elf_glue" "$HERE/test_elf_glue.c" "$HERE/lkl_elf_glue.c"
"$tmp/elf_glue"
gcc -O2 "${warn[@]}" -isystem "$LINUX_DIR/tools/lkl/include" -isystem "$lkl_out/tools/lkl/include" \
    -o "$tmp/macho_shim" "$HERE/test_macho_shim.c" "$HERE/lkl_macho_shim.c"
"$tmp/macho_shim"
