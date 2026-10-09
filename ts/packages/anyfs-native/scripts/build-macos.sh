#!/usr/bin/env bash
# Cross-build anyfs_native.node for macOS (arm64 or x86_64) on Linux with zig.
#
# Usage: scripts/build-macos.sh --arch=arm64|x86_64
#
# node-gyp cannot cross-compile, so this drives the build by hand, like
# build-win64.sh. binding.cc and ts/native/anyfs_ts.c are compiled with the
# repo's zig launchers (scripts/macho/<arch>-macos-cc / -c++: the arch's
# deployment target from scripts/macho/macos_target.sh) and linked with
# everything static except the LKL kernel:
#   - libanyfs_core.a + libanyfs_u8.a  (build_anyfs.sh --targets=macos-<arch>)
#   - liblkl-host.a                    (scripts/macho/build_host_lib.sh)
#   - QEMU's block layer, -force_load  (build_qemu.sh --targets=macos-<arch>)
#   - glib, libblkid, curl, OpenSSL, zstd, bzip2, zlib, libiconv
#                                      (build_macos_sysroot.sh --arch=<arch>)
#   - libc++ (zig's, static)
# and dynamically against @rpath/liblkl-kernel.dylib
# (scripts/macho/build_kernel_dylib.sh), found through LC_RPATH @loader_path:
# the dylib ships next to the .node. napi_* stay undefined and resolve from
# the host process (node or Electron) at load time (-undefined
# dynamic_lookup), as node-gyp links addons on macOS.
#
# Output: build-macos-<arch>/anyfs_native.node and, next to it, a copy of
# liblkl-kernel.dylib. Copy both into the same directory of an app bundle
# (electron-demo: scripts/stage-native-macos.sh). scripts/macho/check_macho.sh
# gates the result: arch, deployment target, load commands, rpath, signature.
#
# Node-API headers: Electron's headers (node-gyp install --runtime=electron),
# version from electron-demo's installed electron unless ELECTRON_TARGET is set.
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$PWD
REPO_ROOT="$(cd ../../.. && pwd)"
# shellcheck source=../../../../scripts/lib/config.sh
source "$REPO_ROOT/scripts/lib/config.sh"

arch=""
for a in "$@"; do
    case "$a" in
        --arch=*) arch="${a#--arch=}" ;;
        -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 1 ;;
    esac
done
case "$arch" in
    arm64)  lkl_dist=linux-arm64 ;;
    x86_64) lkl_dist=linux-amd64 ;;
    *) echo "--arch=arm64|x86_64 is required" >&2; exit 1 ;;
esac

CC="$REPO_ROOT/scripts/macho/$arch-macos-cc"
CXX="$REPO_ROOT/scripts/macho/$arch-macos-c++"
SYS="$ANYFS_PATHS_MACOS_SYSROOT/$arch"
QEMU_SRC="${QEMU_SRC:-$ANYFS_PATHS_QEMU_SRC}"
QEMU_BLD="${QEMU_BLD:-$QEMU_SRC/build-anyfs-macos-$arch}"
LINUX_SRC="${LINUX_SRC:-$ANYFS_PATHS_LINUX_SRC}"
LKL_TREE="$REPO_ROOT/lkl-$lkl_dist"
LKL_MACHO="$REPO_ROOT/build/macos/$arch"
CORE="$REPO_ROOT/build-anyfs-macos-$arch"
OUT="$SRC/build-macos-$arch"
STUBS="$REPO_ROOT/scripts/macho/sdk-stubs/Frameworks"

QEMU_LIBS=(libqemuutil.a libio.a libqom.a libauthz.a libcrypto.a libevent-loop-base.a)
for f in "$CORE/libanyfs_core.a" "$CORE/libanyfs_u8.a" "$LKL_MACHO/liblkl-host.a" \
         "$LKL_MACHO/liblkl-kernel.dylib" "$QEMU_BLD/libblock.a" "${QEMU_LIBS[@]/#/$QEMU_BLD/}" \
         "$SYS/lib/pkgconfig/glib-2.0.pc" "$LKL_TREE/tools/lkl/include/lkl/asm/syscalls.h"; do
    [[ -f "$f" ]] || { echo "build-macos: missing $f" >&2; exit 1; }
done

# Node-API headers, Electron's (same flow as build-linux-electron.sh).
if [[ -z "${ELECTRON_TARGET:-}" ]]; then
    demo_pkg="../../examples/electron-demo/node_modules/electron/package.json"
    if [[ -f "$demo_pkg" ]]; then
        ELECTRON_TARGET="$(node -p "require('$demo_pkg').version")"
    else
        ELECTRON_TARGET="42.3.0"
    fi
fi
devdir="${npm_config_devdir:-${XDG_CACHE_HOME:-$HOME/.cache}/node-gyp}"
if [[ ! -f "$devdir/$ELECTRON_TARGET/include/node/node_api.h" ]]; then
    npx node-gyp install --target="$ELECTRON_TARGET" \
        --dist-url=https://electronjs.org/headers --runtime=electron --devdir="$devdir"
fi
NODE_INC="$devdir/$ELECTRON_TARGET/include/node"
NAPI_INC="$(node -p "require('node-addon-api').include_dir")"

rm -rf "$OUT"
mkdir -p "$OUT"

# Include order as binding.gyp, with the Darwin lkl_autoconf.h profile first
# (it shadows the one generated for a Linux host).
INCS=(
    -I"$NODE_INC" -I"$NAPI_INC"
    -I"$REPO_ROOT/scripts/macho/autoconf"
    -I"$REPO_ROOT/include" -I"$REPO_ROOT/src/core"
    -I"$LKL_TREE/tools/lkl/include"
    -I"$LKL_TREE/arch/lkl/include/generated/uapi"
    -I"$LINUX_SRC/tools/lkl/include" -I"$LINUX_SRC/arch/lkl/include"
)
DEFS=(-DBUILDING_NODE_EXTENSION -DNAPI_DISABLE_CPP_EXCEPTIONS -D_FILE_OFFSET_BITS=64)

echo ">>> anyfs_native.node for macOS $arch (Electron $ELECTRON_TARGET headers)"
"$CXX" -c src/binding.cc -o "$OUT/binding.o" -std=c++17 -O2 -fvisibility=hidden \
    -fvisibility-inlines-hidden "${INCS[@]}" "${DEFS[@]}"
"$CC" -c ../../native/anyfs_ts.c -o "$OUT/anyfs_ts.o" -O2 -fvisibility=hidden \
    "${INCS[@]}" "${DEFS[@]}"

# Static dependency closure from the sysroot's .pc files (curl's Libs.private
# carries the frameworks it calls; -F finds their link stubs).
mapfile -t sys_libs < <(PKG_CONFIG_LIBDIR="$SYS/lib/pkgconfig" \
    pkg-config --static --libs glib-2.0 gthread-2.0 libcurl libzstd zlib blkid | tr ' ' '\n' | sed '/^$/d')

cp "$LKL_MACHO/liblkl-kernel.dylib" "$OUT/liblkl-kernel.dylib"
"$CXX" -shared -o "$OUT/anyfs_native.node" \
    -Wl,-install_name,@rpath/anyfs_native.node \
    -Wl,-undefined,dynamic_lookup \
    -Wl,-rpath,@loader_path -Wl,-dead_strip_dylibs -F"$STUBS" \
    "$OUT/binding.o" "$OUT/anyfs_ts.o" \
    "$CORE/libanyfs_core.a" "$CORE/libanyfs_u8.a" \
    "$LKL_MACHO/liblkl-host.a" "$OUT/liblkl-kernel.dylib" \
    -Wl,-force_load,"$QEMU_BLD/libblock.a" "${QEMU_LIBS[@]/#/$QEMU_BLD/}" \
    "${sys_libs[@]}" -lbz2 -lm

"$REPO_ROOT/scripts/macho/check_macho.sh" --arch="$arch" \
    --dylib=@rpath/liblkl-kernel.dylib --rpath=@loader_path \
    --allow-undefined='^_(napi|node_api)_' "$OUT/anyfs_native.node"
"$REPO_ROOT/scripts/macho/check_macho.sh" --arch="$arch" "$OUT/liblkl-kernel.dylib"
# The N-API entry point, libblkid's renamed crc32c (QEMU's must not replace
# it) and the format drivers -force_load is there for.
# shellcheck source=../../../../scripts/macho/llvm_tools.sh
source "$REPO_ROOT/scripts/macho/llvm_tools.sh"
nm="$(llvm_tool llvm-nm)"
defined="$("$nm" --defined-only "$OUT/anyfs_native.node" | awk '{print $3}')"
for sym in _napi_register_module_v1 _anyfs_blkid_crc32c _bdrv_qcow2 _bdrv_vmdk _bdrv_dmg _dmg_bz2_init; do
    grep -qx "$sym" <<<"$defined" \
        || { echo "build-macos: $sym missing from anyfs_native.node" >&2; exit 1; }
done
rm -f "$OUT"/*.o
echo "Built: $OUT/anyfs_native.node ($(stat -c %s "$OUT/anyfs_native.node") bytes) + liblkl-kernel.dylib"
