#!/bin/bash
# Package anyfs-reader for Linux amd64 distribution
# Usage: ./scripts/package_linux.sh [builddir-dist]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-builddir-dist}"
VERSION="${ANYFS_VERSION:-$(date +%Y%m%d)}"
PACKAGE_NAME="anyfs-reader-${VERSION}-linux-amd64"
STAGING="/tmp/${PACKAGE_NAME}"

echo "=== Packaging anyfs-reader (${PACKAGE_NAME}) ==="

# Verify build directory
if [ ! -d "$SRC_DIR/$BUILD_DIR" ]; then
    echo "ERROR: Build directory '$BUILD_DIR' not found."
    echo "Run: meson setup $BUILD_DIR ... && meson compile -C $BUILD_DIR"
    exit 1
fi

# Install via meson to get proper RUNPATH
echo "--- Running meson install ---"
rm -rf "$STAGING"
meson install -C "$SRC_DIR/$BUILD_DIR" --destdir "$STAGING"

# Find the install prefix (meson uses absolute prefix under destdir)
PREFIX=$(find "$STAGING" -name anyfs-shell -type f -exec dirname {} \;)
PREFIX=$(dirname "$PREFIX")  # go up from bin/

echo "Install prefix: $PREFIX"

# Create clean package layout
PKG="/tmp/${PACKAGE_NAME}-pkg"
rm -rf "$PKG"
mkdir -p "$PKG/bin" "$PKG/lib"

# Copy binaries
for bin in anyfs-gui anyfs-shell anyfs-ksmbd anyfs-nfsd; do
    if [ -f "$PREFIX/bin/$bin" ]; then
        cp "$PREFIX/bin/$bin" "$PKG/bin/"
        echo "  bin/$bin"
    fi
done

# Copy our shared libraries
echo "--- Copying shared libraries ---"
copy_lib() {
    local lib="$1"
    local src
    src=$(ldconfig -p 2>/dev/null | grep "$lib" | head -1 | sed 's/.*=> //' || true)
    if [ -z "$src" ]; then
        # Try direct path
        src="/lib/x86_64-linux-gnu/$lib"
    fi
    if [ -f "$src" ]; then
        cp -L "$src" "$PKG/lib/"
        echo "  lib/$lib (from $src)"
    else
        echo "  WARNING: $lib not found, skipping"
    fi
}

# Our custom shared libs (always bundle)
for lib_path in \
    "$SRC_DIR/$BUILD_DIR/../linux/tools/lkl/lib/liblkl.so" \
    "$HOME/linux/tools/lkl/lib/liblkl.so"; do
    if [ -f "$lib_path" ]; then
        cp -L "$lib_path" "$PKG/lib/"
        echo "  lib/liblkl.so"
        break
    fi
done

# Find libanyfs-qemublk.so from the QEMU build
QEMU_SO=$(find "$HOME/qemu" -name libanyfs-qemublk.so -type f 2>/dev/null | head -1)
if [ -n "$QEMU_SO" ]; then
    cp -L "$QEMU_SO" "$PKG/lib/"
    echo "  lib/libanyfs-qemublk.so"
fi

# Bundle less-common system dependencies
# These may not be installed on target systems
BUNDLE_LIBS=(
    libslirp.so.0
    liburing.so.2
    "libaio.so.1t64"
)
for lib in "${BUNDLE_LIBS[@]}"; do
    copy_lib "$lib"
done

# Set RUNPATH and SONAME on shared libraries so they find each other
echo "--- Setting RUNPATH/SONAME on libraries ---"
for so in "$PKG/lib/"*.so*; do
    if [ -f "$so" ] && file "$so" | grep -q "ELF.*shared"; then
        patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
        # Set SONAME to just the filename (remove absolute path references)
        soname=$(basename "$so")
        patchelf --set-soname "$soname" "$so" 2>/dev/null || true
    fi
done

# Verify RUNPATH on binaries
echo "--- Verifying RUNPATH ---"
for bin in "$PKG/bin/"*; do
    rpath=$(readelf -d "$bin" 2>/dev/null | grep RUNPATH | sed 's/.*\[//' | sed 's/\]//' || true)
    echo "  $(basename "$bin"): RUNPATH=$rpath"
done

# Fix absolute paths in DT_NEEDED (meson links with full path)
echo "--- Fixing DT_NEEDED absolute paths ---"
for bin in "$PKG/bin/"*; do
    # Replace absolute path references with just the filename
    for needed in $(readelf -d "$bin" 2>/dev/null | grep NEEDED | grep -oP '\[/[^\]]+\]' | tr -d '[]'); do
        soname=$(basename "$needed")
        echo "  $(basename "$bin"): $needed -> $soname"
        patchelf --replace-needed "$needed" "$soname" "$bin"
    done
done

# Create tarball
echo "--- Creating tarball ---"
cd /tmp
tar czf "${PACKAGE_NAME}.tar.gz" -C /tmp "${PACKAGE_NAME}-pkg" \
    --transform "s|${PACKAGE_NAME}-pkg|${PACKAGE_NAME}|"

echo ""
echo "=== Package created: /tmp/${PACKAGE_NAME}.tar.gz ==="
ls -lh "/tmp/${PACKAGE_NAME}.tar.gz"

# Show contents
echo ""
echo "=== Contents ==="
tar tzf "/tmp/${PACKAGE_NAME}.tar.gz" | sort

# Cleanup
rm -rf "$STAGING" "$PKG"
