#!/bin/bash
# Build the wasm bundle(s) for @anyfs/core.
#
# Always includes QEMU block backend (qcow2/vmdk/vdi/vhd + raw), ASYNCIFY=1,
# and libblkid-based fstype probing.
#
# Inputs:
#   - $LINUX_DIR                      kernel source tree (default: ~/linux)
#   - $OUT/tools/lkl/liblkl.a         pre-built LKL wasm (build_lkl_wasm.sh)
#   - $HOME/anyfs-reader/src/core/    core C sources
#   - $HOME/anyfs-reader/ts/native/anyfs_ts.c   TypeScript glue
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

LINUX_DIR="${LINUX_DIR:-$HOME/linux}"
OUT="${OUT:-$HOME/anyfs-reader/lkl-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
BLD="${BLD:-$HOME/anyfs-reader/build-anyfs-wasm}"
TS="${TS:-$HOME/anyfs-reader/ts}"
QEMU_ROOT="${QEMU_ROOT:-$HOME/qemu}"
QBLD="${QBLD:-$QEMU_ROOT/build-anyfs-wasm}"
SYS="${WASM_SYSROOT:-$HOME/wasm-sysroot}"
SRC_CORE="${SRC_CORE:-$HOME/anyfs-reader/src/core}"
GLUE="$TS/native/anyfs_ts.c"
LIBLKL="$OUT/tools/lkl/liblkl.a"
TARGET="${ANYFS_TARGET:-browser}"

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

case "$TARGET" in
    browser)
        ENV_FLAG=(-sENVIRONMENT=web,worker)
        FS_FLAG=(-lworkerfs.js)
        FS_RUNTIME="WORKERFS"
        OUT_STEM="anyfs"
        ;;
    node)
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
    -I "$HOME/anyfs-reader/include"
    -I "$SRC_CORE"
)

CFLAGS=(
    -pthread -fno-builtin
    -D_GNU_SOURCE
    -DLKL_HOST_CONFIG_POSIX=1
    -DLKL_HOST_CONFIG_WASM=1
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
    emcc "${CFLAGS[@]}" "${INC[@]}" -c "$SRC_CORE/$src" -o "$obj"
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

# Rebuild anyfs_kernel.c with ANYFS_HAS_QEMU so qemu_backend_ops gets registered.
ANYFS_QEMU_OBJ="$BLD/anyfs.qemu.${TARGET}.o"
echo "  CC   src/core/anyfs_kernel.c (ANYFS_HAS_QEMU) -> $ANYFS_QEMU_OBJ"
emcc "${CFLAGS[@]}" "${INC[@]}" \
     -DANYFS_HAS_QEMU \
     -c "$SRC_CORE/anyfs_kernel.c" -o "$ANYFS_QEMU_OBJ"

QEMU_BLK_OBJ="$BLD/qemu_blk_backend.${TARGET}.o"
echo "  CC   src/core/qemu_backend.c -> $QEMU_BLK_OBJ"
emcc -pthread -O2 -g \
     -I "$SRC_CORE" \
     -I "$HOME/anyfs-reader/include" \
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

# Rebuild anyfs_probe.c with ANYFS_HAS_BLKID so fstype probe succeeds
# without brute-forcing /proc/filesystems (30+ fstypes * ASYNCIFY unwind
# corrupts fiber state).
KINDPROBE_BLKID_OBJ="$BLD/anyfs_probe.blkid.${TARGET}.o"
echo "  CC   src/core/anyfs_probe.c (ANYFS_HAS_BLKID) -> $KINDPROBE_BLKID_OBJ"
emcc "${CFLAGS[@]}" "${INC[@]}" \
     -DANYFS_HAS_BLKID \
     -I "$SYS/include" \
     -c "$SRC_CORE/anyfs_probe.c" -o "$KINDPROBE_BLKID_OBJ"

# ── Phase 4: link ───────────────────────────────────────────────────

echo
echo "=== Phase 4: link ==="

mkdir -p "$TS/packages/core/wasm"
OUT_DIR="$TS/packages/core/wasm"
OUT_JS="$OUT_DIR/${OUT_STEM}.mjs"

EXPORTED_FUNCS='_main,_malloc,_free,'\
'_anyfs_ts_kernel_init,_anyfs_ts_init_async,_anyfs_ts_is_boot_complete,_anyfs_ts_boot_result,_anyfs_ts_kernel_halt,'\
'_anyfs_ts_session_open,_anyfs_ts_session_close,'\
'_anyfs_ts_session_list_json,_anyfs_ts_session_meta_json,'\
'_anyfs_ts_session_enter,'\
'_anyfs_ts_session_enter_async,_anyfs_ts_session_enter_is_complete,_anyfs_ts_session_enter_result_p,'\
'_anyfs_ts_readdir_json,_anyfs_ts_lstat_json,_anyfs_ts_stat_json,'\
'_anyfs_ts_realpath,_anyfs_ts_readlink,_anyfs_ts_read_kernel_file,'\
'_anyfs_ts_open,_anyfs_ts_pread,_anyfs_ts_close,'\
'_anyfs_ts_session_open_p,_anyfs_ts_session_list_json_p,_anyfs_ts_session_meta_json_p,'\
'_anyfs_ts_session_enter_p,'\
'_anyfs_ts_readdir_json_p,_anyfs_ts_lstat_json_p,_anyfs_ts_stat_json_p,'\
'_anyfs_ts_realpath_p,_anyfs_ts_readlink_p,_anyfs_ts_read_kernel_file_p,'\
'_anyfs_ts_open_p,_anyfs_ts_pread_p,_anyfs_ts_close_p'

EXPORTED_RUNTIME="ccall,cwrap,HEAPU8,HEAP32,HEAPU32,FS,${FS_RUNTIME},UTF8ToString,stringToUTF8,getValue,setValue"

EXTRA_OBJS=("$ANYFS_QEMU_OBJ" "$QEMU_BLK_OBJ" "$QEMU_STUBS_OBJ" "$KINDPROBE_BLKID_OBJ")
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
