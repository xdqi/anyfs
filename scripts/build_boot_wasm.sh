#!/bin/bash
# Build tools/lkl/tests/boot for wasm and link with liblkl.a into a JS+wasm
# bundle that Node.js can execute.
#
# Strategy:
#   - Compile tests/boot.c and tests/test.c with emcc directly (don't go
#     through kbuild — its `all` target also pulls in hijack libs and other
#     POSIX-only progs that don't apply to wasm). Skip tests/cla.c: it
#     defines parse_args(int, const char **, struct cl_arg *), which clashes
#     with the kernel's parse_args(8 args) symbol inside lkl.o. ELF/PE LKL
#     side-steps the clash by `objcopy --prefix-symbols=_` on lkl.o; wasm's
#     llvm-objcopy doesn't support that, so we drop cla.o (boot.c doesn't
#     call parse_args).
#   - Link them with the previously-built liblkl.a using emcc.
#   - emcc flags:
#       -pthread                kernel uses pthreads for its main loop
#       -sPROXY_TO_PTHREAD      moves main() to a worker thread so it can
#                               block (atomic.wait can't run on the main
#                               thread under node either)
#       -sPTHREAD_POOL_SIZE=8   pre-spawn workers for LKL's kernel threads
#       -sALLOW_MEMORY_GROWTH   kernel mem=32M plus its own malloc
#       -sINITIAL_MEMORY=64MB   start big to reduce growth churn
#       -sEXIT_RUNTIME=1        node should exit after main returns
#       -sNODERAWFS=0           don't expose host fs (we don't need it)
set -e

# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT="${OUT:-$REPO_ROOT/lkl-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"

LIBLKL="$OUT/tools/lkl/liblkl.a"
if [[ ! -f "$LIBLKL" ]]; then
    echo "Error: $LIBLKL not found. Build lkl-wasm first." >&2
    exit 1
fi

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

TESTS_DIR="$LINUX_DIR/tools/lkl/tests"
INC=(
    -I "$LINUX_DIR/tools/lkl/include"
    -I "$OUT/tools/lkl/include"
)
# Match the CFLAGS used by the kernel/host build so feature flags line up.
CFLAGS=(
    -pthread -fno-builtin
    -D_GNU_SOURCE
    -DLKL_HOST_CONFIG_POSIX=1
    -DLKL_HOST_CONFIG_WASM=1
    -include sys/types.h -include limits.h
    -O2 -g
)

OBJDIR="$OUT/tests-wasm"
mkdir -p "$OBJDIR"

for src in boot.c test.c; do
    obj="$OBJDIR/${src%.c}.o"
    echo "  CC   $src"
    emcc "${CFLAGS[@]}" "${INC[@]}" -c "$TESTS_DIR/$src" -o "$obj"
done

# shellcheck disable=SC2054
# SC2054: false positive — the comma inside -sENVIRONMENT=node,worker is part
# of the emcc flag value, not an array separator.
LDFLAGS=(
    -pthread
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=8
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sMAXIMUM_MEMORY=512MB
    -sEXIT_RUNTIME=1
    -sNODEJS_CATCH_EXIT=0
    -sNODEJS_CATCH_REJECTION=0
    -sENVIRONMENT=node,worker
    -sASSERTIONS=1
    # Keep symbol names + DWARF so OOB traps in the kernel get a readable
    # backtrace. -g3 is emcc's "keep full debuginfo + names section"; without
    # it emcc strips to function indices.
    -g3
)

OUT_JS="$OBJDIR/boot.js"
echo "  LINK $OUT_JS"
# Force-keep liblkl.a content: wasm-ld DCE drops kernel data whose only
# references are script-defined ABSOLUTE bracket symbols (__start_/__stop_).
# When those brackets resolve to a base+offset past the surviving data, the
# kernel iterates uninitialized memory and traps. -Wl,--whole-archive on the
# .a keeps every TU regardless of reference graph.
emcc "${LDFLAGS[@]}" \
    "$OBJDIR/boot.o" "$OBJDIR/test.o" \
    -Wl,--whole-archive "$LIBLKL" -Wl,--no-whole-archive \
    -o "$OUT_JS"

echo
echo "Output:"
ls -lh "$OBJDIR/boot.js" "$OBJDIR/boot.wasm" 2>/dev/null || true
echo
echo "Run: node $OUT_JS"
