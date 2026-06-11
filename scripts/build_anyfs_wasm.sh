#!/bin/bash
# Build the wasm bundle(s) for @anyfs/core.
#
# Always includes QEMU block backend (qcow2/vmdk/vdi/vhd + raw), ASYNCIFY=1,
# and libblkid-based fstype probing.
#
# Inputs:
#   - $LINUX_DIR                      kernel source tree (default: paths.linux_src)
#   - $OUT/tools/lkl/liblkl.a         pre-built LKL wasm (build_lkl_wasm.sh)
#   - <repo>/src/core/                core C sources
#   - <repo>/ts/native/anyfs_ts.c     TypeScript glue
#   - $QEMU_ROOT/build-anyfs-wasm/    pre-built QEMU wasm archives
#   - $WASM_SYSROOT                   sysroot with libblkid/libz/libbz2/libzstd etc.
#
# Output:
#   ANYFS_TARGET=browser (default): ts/packages/core/wasm/anyfs.{mjs,wasm,...}
#   ANYFS_TARGET=node:              ts/packages/core/wasm/anyfs.node.{mjs,wasm,...}
#
# Notes:
#   - QEMU's block layer uses coroutines (emscripten_fiber backend), which
#     forces ASYNCIFY=1. The fiber rewind path discards wasm export return
#     values, so the C glue exposes `_p` (out-pointer) variants for every
#     call that may touch the block layer.
#   - PROXY_TO_PTHREAD=1 so synchronous ccall still works on the main thread;
#     the asyncify unwinding happens on the dedicated pthread.
set -euo pipefail

# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"
# shellcheck source=lib/wasm_exports.sh
source "$(dirname "$0")/lib/wasm_exports.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT="${OUT:-$REPO_ROOT/lkl-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"
BLD="${BLD:-$REPO_ROOT/build-anyfs-wasm}"
TS="${TS:-$REPO_ROOT/ts}"
QEMU_ROOT="${QEMU_ROOT:-$ANYFS_PATHS_QEMU_SRC}"
QBLD="${QBLD:-$QEMU_ROOT/build-anyfs-wasm}"
SYS="${WASM_SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
SRC_CORE="${SRC_CORE:-$REPO_ROOT/src/core}"
TARGET="${ANYFS_TARGET:-browser}"

# Absolutize everything consumed after the `cd "$BLD"` below — CI passes
# LINUX_DIR/QEMU_ROOT as repo-relative paths, which would otherwise
# resolve against the build dir (lkl.h not found, missing archives).
LINUX_DIR="$(cd "$LINUX_DIR" && pwd)"
OUT="$(cd "$OUT" && pwd)"
EMSDK_DIR="$(cd "$EMSDK_DIR" && pwd)"
TS="$(cd "$TS" && pwd)"
QEMU_ROOT="$(cd "$QEMU_ROOT" && pwd)"
QBLD="$(cd "$QBLD" && pwd)"
SYS="$(cd "$SYS" && pwd)"
SRC_CORE="$(cd "$SRC_CORE" && pwd)"
mkdir -p "$BLD"
BLD="$(cd "$BLD" && pwd)"

GLUE="$TS/native/anyfs_ts.c"
LIBLKL="$OUT/tools/lkl/liblkl.a"

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

case "$TARGET" in
    browser)
        # shellcheck disable=SC2054  # comma is part of the emcc value, not an element separator
        ENV_FLAG=(-sENVIRONMENT=web,worker)
        FS_FLAG=(-lworkerfs.js)
        FS_RUNTIME="WORKERFS"
        OUT_STEM="anyfs"
        ;;
    node)
        # shellcheck disable=SC2054  # comma is part of the emcc value, not an element separator
        ENV_FLAG=(-sENVIRONMENT=node,worker)
        FS_FLAG=(-lnodefs.js)
        FS_RUNTIME="NODEFS"
        OUT_STEM="anyfs.node"
        ;;
    *)
        echo "unknown ANYFS_TARGET=$TARGET (browser|node)" >&2
        exit 2
        ;;
esac

INC=(
    -I "$LINUX_DIR/tools/lkl/include"
    -I "$OUT/tools/lkl/include"
    -I "$REPO_ROOT/include"
    -I "$SRC_CORE"
)

CFLAGS=(
    -pthread -fno-builtin
    -D_GNU_SOURCE
    -DLKL_HOST_CONFIG_POSIX=1
    -DLKL_HOST_CONFIG_WASM=1
    # The bundle always links the QEMU block backend, so the whole core
    # must see ANYFS_HAS_QEMU — not just anyfs_kernel.c (which registers
    # qemu_backend_ops). anyfs_backend.c's auto-detect in anyfs_disk_add is
    # #ifdef ANYFS_HAS_QEMU: without the macro here it falls back to the raw
    # backend, so qcow2/vmdk/etc. open as raw bytes (wrong size, no inner
    # partition table). Meson applies -DANYFS_HAS_QEMU=1 to the entire core
    # for the same reason; mirror that here.
    -DANYFS_HAS_QEMU=1
    -include sys/types.h -include limits.h
    -O2 -g
)

# ── Phase 1: build libanyfs_core.a from source ─────────────────────

CORE_SOURCES=(
    anyfs_kernel.c
    anyfs_backend.c
    anyfs_container.c
    anyfs_mount.c
    raw_backend.c
    anyfs_session.c
    anyfs_strbuf.c
    anyfs_format.c
    anyfs_dm.c
    anyfs_sysfs.c
    anyfs_probe.c
    anyfs_path.c
    anyfs_share.c
)

mkdir -p "$BLD"
cd "$BLD"

echo "=== Phase 1: libanyfs_core.a ==="
for src in "${CORE_SOURCES[@]}"; do
    obj="${src%.c}.o"
    echo "  CC   $src"
    # anyfs_probe.c includes <blkid/blkid.h> (always — libblkid is required),
    # so it needs the sysroot include path; the other core files don't.
    extra_inc=()
    [[ "$src" == "anyfs_probe.c" ]] && extra_inc=(-I "$SYS/include")
    emcc "${CFLAGS[@]}" "${INC[@]}" "${extra_inc[@]}" -c "$SRC_CORE/$src" -o "$obj"
done
echo "  AR   libanyfs_core.a"
emar rcs libanyfs_core.a "${CORE_SOURCES[@]/.c/.o}"
echo "  OK   ($(emar t libanyfs_core.a | wc -l) objects)"

# ── Phase 2: compile TS glue ───────────────────────────────────────

echo
echo "=== Phase 2: TS glue ==="

GLUE_OBJ="$BLD/anyfs_ts.${TARGET}.o"
echo "  CC   ts/native/anyfs_ts.c -> $GLUE_OBJ"
emcc "${CFLAGS[@]}" "${INC[@]}" -c "$GLUE" -o "$GLUE_OBJ"

# ── Phase 3: QEMU extras ───────────────────────────────────────────

echo
echo "=== Phase 3: QEMU extras ==="

for f in \
    "$QBLD/libblock.a" "$QBLD/libio.a" "$QBLD/libqom.a" \
    "$QBLD/libauthz.a" "$QBLD/libcrypto.a" \
    "$QBLD/libevent-loop-base.a" "$QBLD/libqemuutil.a"; do
    [[ -f "$f" ]] || { echo "missing QEMU artifact: $f" >&2; exit 1; }
done
for f in \
    "$SYS/lib/libgio-2.0.a" "$SYS/lib/libgmodule-2.0.a" \
    "$SYS/lib/libgobject-2.0.a" "$SYS/lib/libgthread-2.0.a" \
    "$SYS/lib/libglib-2.0.a" \
    "$SYS/lib/libpcre2-8.a" "$SYS/lib/libpcre2-16.a" \
    "$SYS/lib/libpcre2-32.a" \
    "$SYS/lib/libffi.a" "$SYS/lib/libz.a" "$SYS/lib/libresolv.a" \
    "$SYS/lib/libblkid.a" "$SYS/lib/libbz2.a" "$SYS/lib/libzstd.a"; do
    [[ -f "$f" ]] || { echo "missing sysroot lib: $f" >&2; exit 1; }
done

# anyfs_kernel.c (qemu_backend_ops registration) and anyfs_backend.c
# (auto-detect that PREFERS the QEMU backend) are already compiled with
# -DANYFS_HAS_QEMU in Phase 1 — it's in the shared CFLAGS — so they live in
# libanyfs_core.a with the macro on. No separate QEMU rebuild of
# anyfs_kernel.c is needed; the only QEMU-specific translation units left to
# compile here are qemu_backend.c (real QEMU block headers) and qemu_stubs.c.

QEMU_BLK_OBJ="$BLD/qemu_blk_backend.${TARGET}.o"
echo "  CC   src/core/qemu_backend.c -> $QEMU_BLK_OBJ"
emcc -pthread -O2 -g \
     -I "$SRC_CORE" \
     -I "$REPO_ROOT/include" \
     -I "$QEMU_ROOT" -I "$QEMU_ROOT/include" \
     -I "$QBLD" -I "$QBLD/qapi" \
     -I "$SYS/include" -I "$SYS/include/glib-2.0" \
     -I "$SYS/lib/glib-2.0/include" \
     -I "$LINUX_DIR/tools/lkl/include" \
     -I "$OUT/tools/lkl/include" \
     -D_GNU_SOURCE \
     -DLKL_HOST_CONFIG_POSIX=1 -DLKL_HOST_CONFIG_WASM=1 \
     -c "$SRC_CORE/qemu_backend.c" -o "$QEMU_BLK_OBJ"

QEMU_STUBS_OBJ="$BLD/qemu_stubs.${TARGET}.o"
echo "  CC   src/core/qemu_stubs.c -> $QEMU_STUBS_OBJ"
emcc -pthread -O2 -g -c "$SRC_CORE/qemu_stubs.c" -o "$QEMU_STUBS_OBJ"
# anyfs_probe.c is built once into libanyfs_core.a above (with -I $SYS/include
# for <blkid/blkid.h>); libblkid identifies filesystems (fstype/label/uuid +
# the whole-disk hint), avoiding a /proc/filesystems brute-force that ASYNCIFY
# unwinding would corrupt. No separate blkid override object is needed anymore.

# ── Phase 4: link ───────────────────────────────────────────────────

echo
echo "=== Phase 4: link ==="

mkdir -p "$TS/packages/core/wasm"
OUT_DIR="$TS/packages/core/wasm"
OUT_JS="$OUT_DIR/${OUT_STEM}.mjs"

EXPORTED_FUNCS="$(anyfs_wasm_exports "$GLUE")"

EXPORTED_RUNTIME="ccall,cwrap,HEAPU8,HEAP32,HEAPU32,FS,${FS_RUNTIME},UTF8ToString,stringToUTF8,getValue,setValue"

EXTRA_OBJS=("$QEMU_BLK_OBJ" "$QEMU_STUBS_OBJ")
EXTRA_ARCHIVES=(
    "$QBLD/libblock.a" "$QBLD/libio.a" "$QBLD/libqom.a"
    "$QBLD/libauthz.a" "$QBLD/libcrypto.a"
    "$QBLD/libevent-loop-base.a" "$QBLD/libqemuutil.a"
    "$SYS/lib/libgio-2.0.a" "$SYS/lib/libgmodule-2.0.a"
    "$SYS/lib/libgobject-2.0.a" "$SYS/lib/libgthread-2.0.a"
    "$SYS/lib/libglib-2.0.a"
    "$SYS/lib/libpcre2-8.a" "$SYS/lib/libpcre2-16.a"
    "$SYS/lib/libpcre2-32.a"
    "$SYS/lib/libffi.a" "$SYS/lib/libz.a" "$SYS/lib/libresolv.a"
    "$SYS/lib/libblkid.a" "$SYS/lib/libbz2.a" "$SYS/lib/libzstd.a"
)

# shellcheck disable=SC2054  # commas are literal in -Wl,... linker flags, not element separators
LDFLAGS=(
    -pthread
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=32
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sMAXIMUM_MEMORY=512MB
    -sSTACK_SIZE=1048576
    -sDEFAULT_PTHREAD_STACK_SIZE=1048576
    -sEXIT_RUNTIME=0
    -sASSERTIONS=1
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createAnyfsModule
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_FUNCTIONS="$EXPORTED_FUNCS"
    -sEXPORTED_RUNTIME_METHODS="$EXPORTED_RUNTIME"
    -sASYNCIFY=1
    -sASYNCIFY_STACK_SIZE=131072
    -sWASM_BIGINT
    -Wl,--allow-multiple-definition
    -g3
    "${ENV_FLAG[@]}"
    "${FS_FLAG[@]}"
)

echo "  LINK $OUT_JS  (target=$TARGET, qemu=always)"
emcc "${LDFLAGS[@]}" \
    "$GLUE_OBJ" "${EXTRA_OBJS[@]}" libanyfs_core.a "${EXTRA_ARCHIVES[@]}" \
    -Wl,--whole-archive "$LIBLKL" -Wl,--no-whole-archive \
    -o "$OUT_JS"

echo
echo "Output:"
ls -lh "$OUT_DIR/${OUT_STEM}".* 2>/dev/null || true
