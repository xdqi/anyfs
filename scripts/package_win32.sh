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
for exe in anyfs-shell anyfs-ksmbd anyfs-nfsd anyfs-gui; do
    if [[ -f "$BUILDDIR/${exe}.exe" ]]; then
        cp "$BUILDDIR/${exe}.exe" "$OUTDIR/"
        i686-w64-mingw32-strip "$OUTDIR/${exe}.exe"
    fi
done

# DLLs
cp "$LKL_LIB/liblkl.dll" "$OUTDIR/"
cp "$QEMU_BUILD/libanyfs-qemublk.dll" "$OUTDIR/"
cp "$HOME/libslirp/build-mingw32/libslirp-0.dll" "$OUTDIR/"

# MSYS2 runtime DLLs (base + GTK3 GUI)
for dll in \
    libglib-2.0-0.dll libintl-8.dll libiconv-2.dll libpcre2-8-0.dll \
    libgcc_s_dw2-1.dll libwinpthread-1.dll libbz2-1.dll zlib1.dll \
    libzstd.dll libreadline8.dll libtermcap-0.dll \
    libgtk-3-0.dll libgdk-3-0.dll libgdk_pixbuf-2.0-0.dll \
    libgio-2.0-0.dll libgobject-2.0-0.dll libgmodule-2.0-0.dll \
    libcairo-2.dll libcairo-gobject-2.dll libpango-1.0-0.dll \
    libpangocairo-1.0-0.dll libpangoft2-1.0-0.dll libpangowin32-1.0-0.dll \
    libatk-1.0-0.dll libepoxy-0.dll libfribidi-0.dll libharfbuzz-0.dll \
    libfontconfig-1.dll libfreetype-6.dll libpixman-1-0.dll libpng16-16.dll \
    libexpat-1.dll libffi-8.dll libgraphite2.dll libdatrie-1.dll libthai-0.dll \
    libjpeg-8.dll libtiff-6.dll libwebp-7.dll libsharpyuv-0.dll \
    liblzma-5.dll libdeflate.dll libjbig-0.dll libLerc.dll \
    libbrotlidec.dll libbrotlicommon.dll libstdc++-6.dll; do
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
echo "  anyfs-shell.exe  — Interactive filesystem shell"
echo "  anyfs-gui.exe    — GTK3 file manager GUI"
echo "  anyfs-ksmbd.exe  — SMB3 file server"
echo "  anyfs-nfsd.exe   — NFSv4 file server"
echo "  *.dll            — Runtime libraries (LKL + QEMU + GTK3)"
echo ""
echo "Usage on Windows:"
echo "  anyfs-shell.exe disk.img ext4"
echo "  anyfs-gui.exe disk.img"
