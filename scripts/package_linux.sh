#!/bin/bash
# Package anyfs-reader for Linux amd64.
# Usage: ./scripts/package_linux.sh [builddir]   (default build-anyfs-linux-amd64)
#
# Everything is built with zig against the static sysroot (glibc 2.11 floor),
# so the tarball carries only our own shared libraries; check_linux_abi.sh
# gates every ELF before the tarball is written.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

BUILD_DIR="${1:-build-anyfs-linux-amd64}"
VERSION="${ANYFS_VERSION:-$(date +%Y%m%d)}"
PACKAGE_NAME="anyfs-reader-${VERSION}-linux-amd64"
OUT_DIR="${OUT_DIR:-/tmp}"
STAGING="$(mktemp -d)"
PKG="$STAGING/$PACKAGE_NAME"
trap 'rm -rf "$STAGING"' EXIT

LKL_SO="$SRC_DIR/lkl-linux-amd64/tools/lkl/lib/liblkl.so"
QEMU_SO="$ANYFS_PATHS_QEMU_SRC/build-anyfs-linux-amd64/libanyfs-qemublk.so"

echo "=== Packaging $PACKAGE_NAME ==="
[[ -d "$SRC_DIR/$BUILD_DIR" ]] || { echo "ERROR: $BUILD_DIR not found (run scripts/build_anyfs.sh)" >&2; exit 1; }
for f in "$LKL_SO" "$QEMU_SO"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (run build_lkl.sh / build_qemu.sh)" >&2; exit 1; }
done

meson install -C "$SRC_DIR/$BUILD_DIR" --destdir "$STAGING/install" >/dev/null
PREFIX="$(dirname "$(find "$STAGING/install" -name anyfs-ksmbd -type f -print -quit)")/.."

mkdir -p "$PKG/bin" "$PKG/lib"
for bin in anyfs-ksmbd anyfs-nfsd anyfs-lspart anyfs-fuse; do
    if [[ -f "$PREFIX/bin/$bin" ]]; then
        cp "$PREFIX/bin/$bin" "$PKG/bin/"
        echo "  bin/$bin"
    fi
done
cp -L "$LKL_SO" "$QEMU_SO" "$PKG/lib/"
echo "  lib/liblkl.so"
echo "  lib/libanyfs-qemublk.so"

# The libraries find each other next to themselves; give them plain sonames.
for so in "$PKG/lib/"*.so; do
    patchelf --set-rpath '$ORIGIN' "$so"
    patchelf --set-soname "$(basename "$so")" "$so"
done
for bin in "$PKG/bin/"*; do
    echo "  $(basename "$bin"): RUNPATH=$(readelf -d "$bin" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
done

echo "--- ABI gate (glibc 2.11, glibc-only NEEDED) ---"
"$SCRIPT_DIR/check_linux_abi.sh" 2.11 "$PKG"

tar czf "$OUT_DIR/$PACKAGE_NAME.tar.gz" -C "$STAGING" "$PACKAGE_NAME"
echo
echo "=== Package created: $OUT_DIR/$PACKAGE_NAME.tar.gz ==="
ls -lh "$OUT_DIR/$PACKAGE_NAME.tar.gz"
tar tzf "$OUT_DIR/$PACKAGE_NAME.tar.gz" | sort
