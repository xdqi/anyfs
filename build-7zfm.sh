#!/bin/bash
# build-7zfm.sh — Cross-compile 7zFM.exe with LKL for Win32
# Usage: ./build-7zfm.sh [--static] [--package]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-7zfm"
DIST_DIR="${BUILD_DIR}/dist"
LKL_LIB_DIR="${SCRIPT_DIR}/../linux/tools/lkl/lib-win32"

STATIC=OFF
PACKAGE=0

for arg in "$@"; do
    case "$arg" in
        --static) STATIC=ON ;;
        --package) PACKAGE=1 ;;
        --clean) rm -rf "$BUILD_DIR"; echo "Cleaned."; exit 0 ;;
        *) echo "Usage: $0 [--static] [--package] [--clean]"; exit 1 ;;
    esac
done

# Configure
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake "$SCRIPT_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/mingw32-toolchain.cmake" \
    -DLKL_STATIC=$STATIC

# Build
cmake --build . -j"$(nproc)"

# Strip
i686-w64-mingw32-strip 7zFM.exe
echo "Built: $BUILD_DIR/7zFM.exe ($(du -h 7zFM.exe | cut -f1))"

# Package
if [[ $PACKAGE -eq 1 ]]; then
    mkdir -p "$DIST_DIR"
    cp 7zFM.exe "$DIST_DIR/"

    if [[ $STATIC == "OFF" ]]; then
        # Copy LKL DLLs
        cp "$LKL_LIB_DIR"/liblkl.dll "$DIST_DIR/"
        cp "$LKL_LIB_DIR"/libslirp-0.dll "$DIST_DIR/"
        cp "$LKL_LIB_DIR"/libglib-2.0-0.dll "$DIST_DIR/" 2>/dev/null || true
        cp "$LKL_LIB_DIR"/libintl-8.dll "$DIST_DIR/" 2>/dev/null || true
        cp "$LKL_LIB_DIR"/libiconv-2.dll "$DIST_DIR/" 2>/dev/null || true
        cp "$LKL_LIB_DIR"/libpcre2-8-0.dll "$DIST_DIR/" 2>/dev/null || true

        # MinGW runtime (needed by libslirp)
        GCC_LIB=$(dirname "$(i686-w64-mingw32-gcc -print-libgcc-file-name)")
        cp "$GCC_LIB/libgcc_s_dw2-1.dll" "$DIST_DIR/" 2>/dev/null || true
    fi

    # Create archive
    tar czf "$SCRIPT_DIR/7zFM-lkl-win32.tar.gz" -C "$DIST_DIR" .
    echo "Package: $SCRIPT_DIR/7zFM-lkl-win32.tar.gz ($(du -h "$SCRIPT_DIR/7zFM-lkl-win32.tar.gz" | cut -f1))"
    echo "Contents:"
    ls -lh "$DIST_DIR"
fi
