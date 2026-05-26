#!/bin/bash
# Package anyfs-reader Win32 distribution
# Run after: meson build + ninja in builddir-win32
set -e

VERSION="${1:-0.1.0}"
BUILDDIR="$(dirname "$0")/../builddir-win32"
LKL_LIB="$HOME/linux/tools/lkl/lib-win32"
QEMU_BUILD="$HOME/qemu/build-win32"
MSYS2="/opt/msys2/mingw32/bin"
OUTDIR="/tmp/anyfs-win32"
ARCHIVE="anyfs-reader-${VERSION}-win32.tar.gz"

echo "=== Packaging anyfs-reader ${VERSION} for Win32 ==="

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# Executables
for exe in anyfs-ksmbd anyfs-nfsd; do
    if [[ -f "$BUILDDIR/${exe}.exe" ]]; then
        cp "$BUILDDIR/${exe}.exe" "$OUTDIR/"
        i686-w64-mingw32-strip "$OUTDIR/${exe}.exe"
    fi
done

# DLLs
cp "$LKL_LIB/liblkl.dll" "$OUTDIR/"
cp "$QEMU_BUILD/libanyfs-qemublk.dll" "$OUTDIR/"

# MSYS2 runtime DLLs (base only; no GTK3/readline now that GUI/CLI are gone)
for dll in \
    libglib-2.0-0.dll libintl-8.dll libiconv-2.dll libpcre2-8-0.dll \
    libgcc_s_dw2-1.dll libwinpthread-1.dll libbz2-1.dll zlib1.dll \
    libzstd.dll libstdc++-6.dll; do
    if [[ -f "$MSYS2/$dll" ]]; then
        cp "$MSYS2/$dll" "$OUTDIR/"
    fi
done

# Config
cp "$HOME/ksmbd-tools/ksmbd.conf.example" "$OUTDIR/"

# Create archive
cd /tmp
tar czf "$ARCHIVE" anyfs-win32/
echo "=== Created: /tmp/$ARCHIVE ($(du -h "/tmp/$ARCHIVE" | cut -f1)) ==="
echo ""
echo "Contents:"
echo "  anyfs-ksmbd.exe  — SMB3 file server"
echo "  anyfs-nfsd.exe   — NFSv4 file server"
echo "  *.dll            — Runtime libraries (LKL + QEMU)"
