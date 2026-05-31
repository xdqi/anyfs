#!/bin/bash
# Cross-build libblkid.a for mingw (i686 or x86_64).
#
# Inputs:
#   - util-linux source tree (default: $HOME/util-linux; override
#     with UL_SRC=...)
#   - shim headers + TUs under <repo-root>/patches/libblkid/shim/
#
# Output:
#   $OUT_DIR/lib/libblkid.a
#   $OUT_DIR/include/blkid/blkid.h
#   $OUT_DIR/smoke.exe              (only if patches/libblkid/smoke.c exists)
#
# Usage:
#   ./scripts/build_libblkid_mingw.sh mingw64   (default)
#   ./scripts/build_libblkid_mingw.sh mingw32
#
# Why: util-linux's libblkid is not officially supported on Windows. We need
# only superblock magic probing (anyfs_probe.c calls
# blkid_new_probe_from_filename + blkid_do_safeprobe + lookup TYPE/LABEL/UUID),
# so we hand-compile the minimum TU set that the wasm libblkid archive also
# uses, with mingw-specific shims. See patches/libblkid/shim/mingw_shims.c
# for the diagnosis of the _O_BINARY constructor that makes ext4/ext2 probes
# work under mingw's text-mode-by-default msvcrt.

set -euo pipefail

# ---------------------------------------------------------------------------
# Target selection
# ---------------------------------------------------------------------------
TARGET="${1:-mingw64}"
case "$TARGET" in
    mingw64)
        CROSS_PREFIX="${CROSS_PREFIX:-/opt/msys2-cross/bin/x86_64-w64-mingw32}"
        ;;
    mingw32)
        CROSS_PREFIX="${CROSS_PREFIX:-/opt/msys2-cross/bin/i686-w64-mingw32}"
        ;;
    *)
        echo "Usage: $0 [mingw32|mingw64]" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

UL_SRC="${UL_SRC:-$HOME/util-linux}"
# Resolve UL_SRC to absolute: we `cd "$UL_SRC"` below and pass -I"$UL_SRC/...",
# so a relative UL_SRC (e.g. deps/util-linux) would break after the cd.
[[ -d "$UL_SRC" ]] && UL_SRC="$(cd "$UL_SRC" && pwd)"
SHIM="${SHIM:-$REPO_ROOT/patches/libblkid/shim}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/build-blkid-$TARGET}"
# Resolve OUT_DIR to absolute since we `cd "$UL_SRC"` below for compilation.
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

OBJS="$OUT_DIR/objs"
LIB="$OUT_DIR/lib"
INC="$OUT_DIR/include"

CC="${CC:-${CROSS_PREFIX}-gcc}"
AR="${AR:-${CROSS_PREFIX}-ar}"
RANLIB="${RANLIB:-${CROSS_PREFIX}-ranlib}"

for f in "$UL_SRC/libblkid/src/blkid.h" "$SHIM/config.h" "$SHIM/mingw_shims.c"; do
    if [[ ! -f "$f" ]]; then
        echo "missing required input: $f" >&2
        exit 1
    fi
done
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "compiler not found: $CC" >&2
    exit 1
fi

mkdir -p "$OBJS" "$LIB" "$INC/blkid"

# Stage the generated blkid.h. util-linux's autotools normally produces this
# from blkid.h.in by substituting @LIBBLKID_VERSION@/@LIBBLKID_DATE@; the
# committed blkid.h in the source tree already has those filled in by an
# earlier configure run, so we just copy it.
cp "$UL_SRC/libblkid/src/blkid.h" "$INC/blkid/blkid.h"

CFLAGS=(
    -include "$SHIM/config.h"
    -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
    -O2 -fno-strict-aliasing
    -Wall
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
    -Wno-sign-compare -Wno-pointer-sign -Wno-implicit-function-declaration
    -Wno-format -Wno-format-extra-args -Wno-int-to-pointer-cast
    -Wno-discarded-qualifiers -Wno-missing-braces -Wno-stringop-truncation
    -Wno-stringop-overflow -Wno-array-bounds
    -I"$SHIM" -I"$UL_SRC" -I"$UL_SRC/include"
    -I"$UL_SRC/libblkid/src"
)

# ---------------------------------------------------------------------------
# Source list — same TUs as the wasm libblkid archive. Keeps every
# superblocks/* and partitions/* prober compilable on mingw; the runtime call
# chain stays the same as the Linux build, but lib-common entries that need
# Linux-only APIs are dropped (see explanatory list below). If a future
# libblkid release adds new deps, the link step will surface them.
# ---------------------------------------------------------------------------
CORE=(
    libblkid/src/init.c
    libblkid/src/cache.c
    libblkid/src/config.c
    libblkid/src/dev.c
    libblkid/src/devname.c
    libblkid/src/devno.c
    libblkid/src/encode.c
    libblkid/src/evaluate.c
    libblkid/src/getsize.c
    libblkid/src/probe.c
    libblkid/src/read.c
    libblkid/src/resolve.c
    libblkid/src/save.c
    libblkid/src/tag.c
    libblkid/src/verify.c
    libblkid/src/version.c
)

PARTS=(
    libblkid/src/partitions/aix.c
    libblkid/src/partitions/atari.c
    libblkid/src/partitions/bsd.c
    libblkid/src/partitions/dos.c
    libblkid/src/partitions/gpt.c
    libblkid/src/partitions/mac.c
    libblkid/src/partitions/partitions.c
    libblkid/src/partitions/sgi.c
    libblkid/src/partitions/solaris_x86.c
    libblkid/src/partitions/sun.c
    libblkid/src/partitions/ultrix.c
    libblkid/src/partitions/unixware.c
)
# partitions/minix.c collides with superblocks/minix.c on basename, so it
# gets compiled separately to a distinct object filename.
PARTS_MINIX=libblkid/src/partitions/minix.c

SUPERB=(
    libblkid/src/superblocks/adaptec_raid.c
    libblkid/src/superblocks/apfs.c
    libblkid/src/superblocks/bcache.c
    libblkid/src/superblocks/befs.c
    libblkid/src/superblocks/bfs.c
    libblkid/src/superblocks/bitlocker.c
    libblkid/src/superblocks/bluestore.c
    libblkid/src/superblocks/btrfs.c
    libblkid/src/superblocks/cs_fvault2.c
    libblkid/src/superblocks/cramfs.c
    libblkid/src/superblocks/ddf_raid.c
    libblkid/src/superblocks/drbd.c
    libblkid/src/superblocks/drbdproxy_datalog.c
    libblkid/src/superblocks/drbdmanage.c
    libblkid/src/superblocks/exfat.c
    libblkid/src/superblocks/exfs.c
    libblkid/src/superblocks/ext.c
    libblkid/src/superblocks/f2fs.c
    libblkid/src/superblocks/gfs.c
    libblkid/src/superblocks/hfs.c
    libblkid/src/superblocks/highpoint_raid.c
    libblkid/src/superblocks/hpfs.c
    libblkid/src/superblocks/iso9660.c
    libblkid/src/superblocks/isw_raid.c
    libblkid/src/superblocks/jfs.c
    libblkid/src/superblocks/jmicron_raid.c
    libblkid/src/superblocks/linux_raid.c
    libblkid/src/superblocks/lsi_raid.c
    libblkid/src/superblocks/luks.c
    libblkid/src/superblocks/lvm.c
    libblkid/src/superblocks/minix.c
    libblkid/src/superblocks/mpool.c
    libblkid/src/superblocks/netware.c
    libblkid/src/superblocks/nilfs.c
    libblkid/src/superblocks/ntfs.c
    libblkid/src/superblocks/refs.c
    libblkid/src/superblocks/nvidia_raid.c
    libblkid/src/superblocks/ocfs.c
    libblkid/src/superblocks/promise_raid.c
    libblkid/src/superblocks/reiserfs.c
    libblkid/src/superblocks/romfs.c
    libblkid/src/superblocks/silicon_raid.c
    libblkid/src/superblocks/squashfs.c
    libblkid/src/superblocks/stratis.c
    libblkid/src/superblocks/superblocks.c
    libblkid/src/superblocks/swap.c
    libblkid/src/superblocks/sysv.c
    libblkid/src/superblocks/ubi.c
    libblkid/src/superblocks/ubifs.c
    libblkid/src/superblocks/udf.c
    libblkid/src/superblocks/ufs.c
    libblkid/src/superblocks/vdo.c
    libblkid/src/superblocks/vfat.c
    libblkid/src/superblocks/via_raid.c
    libblkid/src/superblocks/vmfs.c
    libblkid/src/superblocks/vxfs.c
    libblkid/src/superblocks/xfs.c
    libblkid/src/superblocks/zfs.c
    libblkid/src/superblocks/zonefs.c
    libblkid/src/superblocks/erofs.c
)

TOPO=(
    libblkid/src/topology/topology.c
)

# libcommon TUs we keep. Dropped from the wasm-archive's list because mingw
# cannot compile them, and the runtime call chain doesn't reach their
# symbols when libblkid is pointed at a regular file:
#   blkdev.c, canonicalize.c, cpuset.c, fileutils.c, idcache.c, path.c,
#   procfs.c, pwdutils.c, signames.c, sysfs.c, timeutils.c, ttyutils.c
LIBCOMMON=(
    lib/buffer.c
    lib/color-names.c
    lib/crc32.c
    lib/crc32c.c
    lib/crc64.c
    lib/c_strtod.c
    lib/encode.c
    lib/env.c
    lib/jsonwrt.c
    lib/mangle.c
    lib/match.c
    lib/mbsalign.c
    lib/mbsedit.c
    lib/md5.c
    lib/randutils.c
    lib/sha1.c
    lib/sha256.c
    lib/strutils.c
    lib/strv.c
    lib/xxhash.c
)

SOURCES=("${CORE[@]}" "${PARTS[@]}" "${SUPERB[@]}" "${TOPO[@]}" "${LIBCOMMON[@]}")

cd "$UL_SRC"
rm -f "$OBJS"/*.o "$LIB/libblkid.a"

echo ">>> Building libblkid for $TARGET (CC=$CC)"
echo "    util-linux src: $UL_SRC"
echo "    output:         $OUT_DIR"

FAILED=()
for src in "${SOURCES[@]}"; do
    # Disambiguate basename collisions (lib/encode.c vs
    # libblkid/src/encode.c).
    case "$src" in
        libblkid/src/encode.c)           obj="$OBJS/blkid_encode.o";;
        lib/encode.c)                    obj="$OBJS/lib_encode.o";;
        *) obj="$OBJS/$(basename "${src%.c}").o";;
    esac
    if ! "$CC" -c "${CFLAGS[@]}" "$src" -o "$obj" 2>"$obj.err"; then
        echo "    FAILED ($src):"
        sed -n '1,30p' "$obj.err" | sed 's/^/      /'
        FAILED+=("$src")
        continue
    fi
    rm -f "$obj.err"
done

# Compile the partition-table-minix.c separately to avoid basename clash with
# superblocks/minix.c.
"$CC" -c "${CFLAGS[@]}" "$PARTS_MINIX" -o "$OBJS/pt_minix.o"

# Compile mingw shim TU. Provides:
#   - the _CRT_fmode = _O_BINARY weak + blkid_shim_force_binary constructor
#     that flips msvcrt to binary mode before any open() in libblkid runs.
#     Without it, ext4/ext2 superblocks short-read at the first 0x1A byte
#     and TYPE detection fails silently.
#   - tombstone implementations for the libcommon helpers we dropped.
"$CC" -c "${CFLAGS[@]}" "$SHIM/mingw_shims.c" -o "$OBJS/mingw_shims.o"

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "FAILED:"
    for f in "${FAILED[@]}"; do echo "  $f"; done
    exit 1
fi

"$AR" rcs "$LIB/libblkid.a" "$OBJS"/*.o
"$RANLIB" "$LIB/libblkid.a"

echo "Done. $LIB/libblkid.a"
ls -la "$LIB/libblkid.a"

# Optional smoke. Lives under patches/ so the recipe is checked in.
SMOKE_SRC="$REPO_ROOT/patches/libblkid/smoke.c"
if [[ -f "$SMOKE_SRC" ]]; then
    cp "$SMOKE_SRC" "$OUT_DIR/smoke.c"
    "$CC" -g -O2 -I "$INC" "$OUT_DIR/smoke.c" "$LIB/libblkid.a" \
          -lwinpthread -lws2_32 -static -o "$OUT_DIR/smoke.exe"
    echo "  -> $OUT_DIR/smoke.exe"
fi
