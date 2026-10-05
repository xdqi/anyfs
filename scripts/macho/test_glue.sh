#!/bin/bash
# Native (Linux) unit tests for the two sides of the ELF/Mach-O boundary:
# lkl_elf_glue.c and lkl_macho_shim.c. Neither is Darwin-specific C, so both
# build with the host gcc.
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

gcc -O2 -Wall -Wextra -o "$tmp/elf_glue" "$HERE/test_elf_glue.c" "$HERE/lkl_elf_glue.c"
"$tmp/elf_glue"
gcc -O2 -Wall -Wextra -I"$LINUX_DIR/tools/lkl/include" -I"$lkl_out/tools/lkl/include" \
    -o "$tmp/macho_shim" "$HERE/test_macho_shim.c" "$HERE/lkl_macho_shim.c"
"$tmp/macho_shim"
