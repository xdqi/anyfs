#!/bin/bash
# Build the browser-targeted wasm bundle for @anyfs/core.
#
# Inputs:
#   - $OUT/tools/lkl/liblkl.a               (existing — built by build_lkl_wasm.sh)
#   - $HOME/anyfs-reader/build-anyfs-wasm/libanyfs_core.a   (existing)
#   - $HOME/anyfs-reader/ts/native/anyfs_ts.c
#
# Output:
#   ANYFS_QEMU=0 (default): ts/packages/core/wasm/anyfs.{mjs,wasm,worker.mjs}
#                           — raw images only (~10 MiB wasm)
#   ANYFS_QEMU=1:           ts/packages/core/wasm/anyfs.qemu.{mjs,wasm,...}
#                           — qcow2/vmdk/vdi/vhd + raw via QEMU libblock
#                             (~25 MiB wasm, +ASYNCIFY)
#
# Variants:
#   ANYFS_TARGET=browser  default. -sENVIRONMENT=web,worker + -lworkerfs.js
#   ANYFS_TARGET=node     for the Node smoke test. -sENVIRONMENT=node,worker
#                         + -lnodefs.js; output suffix .node.{mjs,wasm,...}
#
# Notes on the QEMU bundle:
#   - QEMU's block layer uses coroutines (emscripten_fiber backend), which
#     forces ASYNCIFY=1. The fiber rewind path discards wasm export return
#     values, so the C glue exposes `_p` (out-pointer) variants for every
#     call that may touch the block layer. See project-qemu-wasm-port memory.
#   - util/qemu-timer.c is patched locally to use emscripten_sleep instead
#     of poll() (emscripten's poll fallback spin-loops on the same thread).
#   - libanyfs_core.a must be rebuilt with -DANYFS_HAS_QEMU before invoking
#     this script with ANYFS_QEMU=1 (so anyfs.c registers qemu_backend_ops).
set -euo pipefail

LINUX_DIR="${LINUX_DIR:-$HOME/linux}"
OUT="${OUT:-$HOME/anyfs-reader/lkl-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
BLD="${BLD:-$HOME/anyfs-reader/build-anyfs-wasm}"
TS="${TS:-$HOME/anyfs-reader/ts}"
QBLD="${QBLD:-$HOME/qemu/build-anyfs-wasm}"
SYS="${WASM_SYSROOT:-$HOME/wasm-sysroot}"
SRC_CORE="${SRC_CORE:-$HOME/anyfs-reader/src/core}"
GLUE="$TS/native/anyfs_ts.c"
LIBLKL="$OUT/tools/lkl/liblkl.a"
LIBCORE="$BLD/libanyfs_core.a"
TARGET="${ANYFS_TARGET:-browser}"
USE_QEMU="${ANYFS_QEMU:-0}"

for f in "$LIBLKL" "$LIBCORE" "$GLUE"; do
    [[ -f "$f" ]] || { echo "missing: $f" >&2; exit 1; }
done

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

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

case "$TARGET" in
    browser)
        ENV_FLAG=(-sENVIRONMENT=web,worker)
        FS_FLAG=(-lworkerfs.js)
        FS_RUNTIME="WORKERFS"
        ;;
    node)
        ENV_FLAG=(-sENVIRONMENT=node,worker)
        FS_FLAG=(-lnodefs.js)
        FS_RUNTIME="NODEFS"
        ;;
    *)
        echo "unknown ANYFS_TARGET=$TARGET (browser|node)" >&2
        exit 2
        ;;
esac

OUT_DIR="$TS/packages/core/wasm"
if [[ "$USE_QEMU" == "1" ]]; then
    OUT_STEM="anyfs.qemu"
    [[ "$TARGET" == "node" ]] && OUT_STEM="anyfs.qemu.node"
else
    OUT_STEM="anyfs"
    [[ "$TARGET" == "node" ]] && OUT_STEM="anyfs.node"
fi

mkdir -p "$OUT_DIR"
GLUE_OBJ="$BLD/anyfs_ts.${TARGET}.o"

echo "  CC   ts/native/anyfs_ts.c -> $GLUE_OBJ"
emcc "${CFLAGS[@]}" "${INC[@]}" -c "$GLUE" -o "$GLUE_OBJ"

# Non-_p variants are still needed for boot/mount ops that never touch
# the asyncify path (init, kernel_halt, disk_close).
# _p variants are needed for any op that may go through QEMU asyncify fibers.
EXPORTED_FUNCS='_main,_malloc,_free,'\
'_anyfs_ts_init,_anyfs_ts_init_async,_anyfs_ts_is_boot_complete,_anyfs_ts_boot_result,_anyfs_ts_kernel_halt,'\
'_anyfs_ts_disk_open,_anyfs_ts_disk_close,'\
'_anyfs_ts_disk_list_json,_anyfs_ts_disk_meta_json,'\
'_anyfs_ts_disk_enter,_anyfs_ts_mount_whole,'\
'_anyfs_ts_readdir_json,_anyfs_ts_lstat_json,_anyfs_ts_stat_json,'\
'_anyfs_ts_realpath,_anyfs_ts_readlink,_anyfs_ts_read_kernel_file,'\
'_anyfs_ts_open,_anyfs_ts_pread,_anyfs_ts_close,'\
'_anyfs_ts_disk_open_p,_anyfs_ts_disk_list_json_p,_anyfs_ts_disk_meta_json_p,'\
'_anyfs_ts_mount_whole_p,_anyfs_ts_disk_enter_p,'\
'_anyfs_ts_readdir_json_p,_anyfs_ts_lstat_json_p,_anyfs_ts_stat_json_p,'\
'_anyfs_ts_realpath_p,_anyfs_ts_readlink_p,_anyfs_ts_read_kernel_file_p,'\
'_anyfs_ts_open_p,_anyfs_ts_pread_p,_anyfs_ts_close_p'

EXPORTED_RUNTIME="ccall,cwrap,HEAPU8,HEAP32,HEAPU32,FS,${FS_RUNTIME},UTF8ToString,stringToUTF8,getValue,setValue"

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
    -g3
    "${ENV_FLAG[@]}"
    "${FS_FLAG[@]}"
)

EXTRA_OBJS=()
EXTRA_ARCHIVES=()

if [[ "$USE_QEMU" == "1" ]]; then
    # Verify QEMU + sysroot artifacts exist.
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

    # Rebuild anyfs.c with ANYFS_HAS_QEMU so qemu_backend_ops gets registered.
    ANYFS_QEMU_OBJ="$BLD/anyfs.qemu.${TARGET}.o"
    echo "  CC   src/core/anyfs.c (ANYFS_HAS_QEMU) -> $ANYFS_QEMU_OBJ"
    emcc "${CFLAGS[@]}" "${INC[@]}" \
         -DANYFS_HAS_QEMU \
         -c "$SRC_CORE/anyfs.c" -o "$ANYFS_QEMU_OBJ"

    # qemu_backend.c needs QEMU headers (qemu/osdep.h etc.) and sysroot.
    QEMU_BLK_OBJ="$BLD/qemu_blk_backend.${TARGET}.o"
    echo "  CC   src/core/qemu_backend.c -> $QEMU_BLK_OBJ"
    emcc -pthread -O2 -g \
         -I "$SRC_CORE" \
         -I "$HOME/anyfs-reader/include" \
         -I "$HOME/qemu" -I "$HOME/qemu/include" \
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

    # Rebuild kindprobe with ANYFS_HAS_BLKID so anyfs_ts_mount_whole's
    # fstype probe succeeds and avoids the brute-force /proc/filesystems
    # mount loop (30+ fstypes * ASYNCIFY unwind corrupts fiber state).
    KINDPROBE_BLKID_OBJ="$BLD/anyfs_probe.blkid.${TARGET}.o"
    echo "  CC   src/core/anyfs_probe.c (ANYFS_HAS_BLKID) -> $KINDPROBE_BLKID_OBJ"
    emcc "${CFLAGS[@]}" "${INC[@]}" \
         -DANYFS_HAS_BLKID \
         -I "$SYS/include" \
         -c "$SRC_CORE/anyfs_probe.c" -o "$KINDPROBE_BLKID_OBJ"

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
    LDFLAGS+=(
        -sASYNCIFY=1
        -sASYNCIFY_STACK_SIZE=131072
        -sWASM_BIGINT
        -Wl,--allow-multiple-definition
    )
fi

OUT_JS="$OUT_DIR/${OUT_STEM}.mjs"
echo "  LINK $OUT_JS  (target=$TARGET, qemu=$USE_QEMU)"
emcc "${LDFLAGS[@]}" \
    "$GLUE_OBJ" "${EXTRA_OBJS[@]}" "$LIBCORE" "${EXTRA_ARCHIVES[@]}" \
    -Wl,--whole-archive "$LIBLKL" -Wl,--no-whole-archive \
    -o "$OUT_JS"

echo
echo "Output:"
ls -lh "$OUT_DIR/${OUT_STEM}".* 2>/dev/null || true
