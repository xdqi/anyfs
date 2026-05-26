#!/usr/bin/env bash
# Cross-build anyfs_native.node (win64) for Electron using the msys2-cross
# mingw-w64 toolchain. binding.gyp can't easily cross-compile, so we drive
# the build by hand.
#
# Statically links libanyfs_core.a, then dynamically links against three
# pre-built DLLs that anyfs-ksmbd.exe already uses:
#   - liblkl.dll                 (LKL kernel; 353 MB static .a overflows
#                                 PE32+ 32-bit relocs)
#   - libanyfs-qemublk.dll       (QEMU block layer + format drivers)
#   - libslirp.dll               (TCP/IP host glue)
# Plus glib-2.0.dll.a from msys2-cross.
#
# napi symbols come from node.lib (electron's import lib). We link with
# `ld.lld --delayload=node.exe` so the resulting PE has node.exe in its
# *delay* import directory — no real node.exe DLL is needed at runtime.
# binding.cc registers __pfnDliNotifyHook2 to redirect node.exe lookups
# to GetModuleHandle(NULL), so napi_* symbols are resolved against the
# host EXE (electron.exe / anyfs-demo.exe). See:
# [[mingw-delayload-lld]] in memory for why this works.
#
# Output:
#   build-win64/anyfs_native.node       (PE32+ x86_64)
#
# Distribution layout (matches anyfs-ksmbd.exe deliverable):
#   <electron-app>/anyfs_native.node
#   <electron-app>/liblkl.dll                (copy from lkl-mingw64/tools/lkl/lib/)
#   <electron-app>/libanyfs-qemublk.dll      (copy from qemu/build-anyfs-mingw64/)
#   <electron-app>/libslirp-0.dll            (copy from msys2-cross/mingw64/bin/)
#   <electron-app>/libglib-2.0-0.dll         (copy from msys2-cross/mingw64/bin/)
#   <electron-app>/libwinpthread-1.dll       (copy from msys2-cross/mingw64/bin/)
#   <electron-app>/libgcc_s_seh-1.dll, libstdc++-6.dll, libintl-8.dll, libiconv-2.dll
#                                            (copy from msys2-cross/mingw64/bin/)
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$PWD

ELECTRON_VERSION=${ELECTRON_VERSION:-42.2.0}
HEADER_CACHE=${HEADER_CACHE:-/tmp/electron-build}
CROSS_PREFIX=${CROSS_PREFIX:-/opt/msys2-cross/bin/x86_64-w64-mingw32-}
MINGW_SYSROOT=${MINGW_SYSROOT:-/opt/msys2-cross/mingw64}

# Repo-relative inputs (same anchors as binding.gyp uses)
REPO_ROOT=${REPO_ROOT:-$SRC/../../..}
LINUX_SRC=${LINUX_SRC:-$HOME/linux}
LIBSLIRP_SRC=${LIBSLIRP_SRC:-$HOME/libslirp}
QEMU_SRC=${QEMU_SRC:-$HOME/qemu}
QEMU_BLD_MINGW64=${QEMU_BLD_MINGW64:-$HOME/qemu/build-anyfs-mingw64}

LKL_MINGW64=${LKL_MINGW64:-$REPO_ROOT/lkl-mingw64}
ANYFS_CORE_MINGW64=${ANYFS_CORE_MINGW64:-$REPO_ROOT/build-anyfs-mingw64/libanyfs_core.a}
LIBSLIRP_MINGW=${LIBSLIRP_MINGW:-$LIBSLIRP_SRC/build-mingw32-static/libslirp.a}
LIBBLKID_MINGW64=${LIBBLKID_MINGW64:-$REPO_ROOT/build-blkid-mingw64/lib/libblkid.a}

OUT=$SRC/build-win64

CC=${CROSS_PREFIX}gcc
CXX=${CROSS_PREFIX}g++

# 1. Fetch electron headers + node.lib (cached)
mkdir -p "$HEADER_CACHE"
if [ ! -d "$HEADER_CACHE/node_headers" ]; then
  curl -sSL -o "$HEADER_CACHE/node-headers.tar.gz" \
    "https://electronjs.org/headers/v${ELECTRON_VERSION}/node-v${ELECTRON_VERSION}-headers.tar.gz"
  tar -xzf "$HEADER_CACHE/node-headers.tar.gz" -C "$HEADER_CACHE"
fi
if [ ! -f "$HEADER_CACHE/node.lib" ]; then
  curl -sSL -o "$HEADER_CACHE/node.lib" \
    "https://electronjs.org/headers/v${ELECTRON_VERSION}/win-x64/node.lib"
fi

# 2. Sanity check inputs (DLL recipe — see header comment)
for f in \
    "$ANYFS_CORE_MINGW64" \
    "$LKL_MINGW64/tools/lkl/lib/liblkl.dll" \
    "$QEMU_BLD_MINGW64/libanyfs-qemublk.dll" \
    "$MINGW_SYSROOT/lib/libslirp.dll.a" \
    "$MINGW_SYSROOT/lib/libglib-2.0.dll.a" \
    "$LIBBLKID_MINGW64"; do
  [[ -f "$f" ]] || { echo "missing input: $f" >&2; exit 1; }
done

mkdir -p "$OUT"

# 3. Compile flags shared by binding.cc + anyfs_ts.c
INCS=(
  -I "$HEADER_CACHE/node_headers/include/node"
  -I "$SRC/node_modules/node-addon-api"
  -I "$SRC/src"
  -I "$REPO_ROOT/include"
  -I "$REPO_ROOT/include/mingw-shims"
  -I "$REPO_ROOT/src/core"
  -I "$LINUX_SRC/tools/lkl/include/mingw32"
  -I "$LKL_MINGW64/tools/lkl/include"
  -I "$LKL_MINGW64/arch/lkl/include/generated/uapi"
  -I "$LINUX_SRC/tools/lkl/include"
  -I "$LINUX_SRC/arch/lkl/include"
  -I "$MINGW_SYSROOT/include"
  -I "$MINGW_SYSROOT/include/glib-2.0"
  -I "$MINGW_SYSROOT/lib/glib-2.0/include"
  -isystem "$QEMU_SRC/linux-headers"
)

DEFINES=(
  -DBUILDING_NODE_EXTENSION
  -DNAPI_VERSION=8
  -DNAPI_DISABLE_CPP_EXCEPTIONS
  -DUNICODE -D_UNICODE
  -DWIN32 -D_WINDOWS
  -D_FILE_OFFSET_BITS=64
)

CXXFLAGS=(
  -std=c++17 -O2 -g
  -fpermissive
  -Wno-unused-parameter
  -Wno-missing-field-initializers
)

CFLAGS=(
  -O2 -g
  -Wno-unused-parameter
)

# 4. Compile
echo ">>> Compiling binding.cc"
$CXX -c "$SRC/src/binding.cc"            "${INCS[@]}" "${DEFINES[@]}" "${CXXFLAGS[@]}" -o "$OUT/binding.o"
echo ">>> Compiling anyfs_ts.c"
$CC  -c "$SRC/../../native/anyfs_ts.c"   "${INCS[@]}" "${DEFINES[@]}" "${CFLAGS[@]}"  -o "$OUT/anyfs_ts.o"

# 5. Link as .node (PE32+ DLL). Pattern mirrors anyfs-ksmbd.exe's link line:
#    libanyfs_core.a statically; LKL / QEMU-block / slirp / glib all as DLLs.
#    --export-all-symbols keeps napi_register_module_v1 visible by name.
#    Linker is ld.lld so we can use --delayload=node.exe; binding.cc
#    redirects the helper to GetModuleHandle(NULL).
LLD_FAKEBIN=${LLD_FAKEBIN:-/opt/msys2-cross/lld-bin}
[[ -x "$LLD_FAKEBIN/ld.lld" ]] || {
    echo "error: $LLD_FAKEBIN/ld.lld missing — run:" >&2
    echo "  mkdir -p $LLD_FAKEBIN && ln -sf \$(which ld.lld-20 || which ld.lld) $LLD_FAKEBIN/ld.lld" >&2
    exit 1
}

echo ">>> Linking anyfs_native.node (ld.lld, --delayload=node.exe)"
$CXX -shared -o "$OUT/anyfs_native.node" \
  -B"$LLD_FAKEBIN" -fuse-ld=lld \
  "$OUT/binding.o" "$OUT/anyfs_ts.o" \
  -L"$MINGW_SYSROOT/lib" \
  -Wl,--start-group \
    "$ANYFS_CORE_MINGW64" \
    "$LIBBLKID_MINGW64" \
    "$LKL_MINGW64/tools/lkl/lib/liblkl.dll" \
    "$QEMU_BLD_MINGW64/libanyfs-qemublk.dll" \
    "$MINGW_SYSROOT/lib/libslirp.dll.a" \
    "$MINGW_SYSROOT/lib/libglib-2.0.dll.a" \
    "$MINGW_SYSROOT/lib/libintl.dll.a" \
  -Wl,--end-group \
  "$HEADER_CACHE/node.lib" \
  -Wl,--delayload=node.exe \
  -pthread \
  -lsynchronization -lws2_32 -liphlpapi \
  -lkernel32 -ladvapi32 -lole32 -luuid -lshell32 -luser32 \
  -static-libgcc -static-libstdc++ \
  -Wl,--export-all-symbols

echo
echo "Built: $(file $OUT/anyfs_native.node | head -1)"
echo "Size : $(stat -c %s $OUT/anyfs_native.node) bytes"
