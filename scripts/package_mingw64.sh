#!/bin/bash
# Package anyfs-reader Win64 distribution.
# Run after: meson setup + ninja in build-anyfs-mingw64.
#
# The build's bin/ subdir is already a symlink farm covering the full
# runtime dependency closure (liblkl, libglib, libwinpthread, libpcre2,
# libintl, libiconv, libanyfs-qemublk, libbz2, libzstd, zlib).
# We just dereference-copy from there, strip everything, and tar it up.
set -euo pipefail

VERSION="${1:-0.1.0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="${BUILDDIR:-$SCRIPT_DIR/../build-anyfs-mingw64}"
STRIP="${STRIP:-x86_64-w64-mingw32-strip}"
OUTDIR="${OUTDIR:-/tmp/anyfs-win64}"
ARCHIVE="anyfs-reader-${VERSION}-win64.tar.gz"

if [[ ! -d "$BUILDDIR/bin" ]]; then
    echo "error: $BUILDDIR/bin not found — run meson + ninja first" >&2
    exit 1
fi

echo "=== Packaging anyfs-reader ${VERSION} for Win64 ==="
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# Dereference-copy real .exe + .dll out of the symlink farm.
#
# Excluded:
#   test_*.exe       — dev-only, not user-facing
for f in "$BUILDDIR"/bin/*.exe "$BUILDDIR"/bin/*.dll; do
    [[ -e "$f" ]] || continue
    name="$(basename "$f")"
    case "$name" in
        test_*) continue ;;
    esac
    cp -L "$f" "$OUTDIR/$name"
done

# Pull in other user-facing exes that live outside bin/.
# anyfs-lspart's imports (kernel32, msvcrt, libwinpthread, liblkl) are
# all covered by the DLLs above.
EXTRA_EXES=( "src/lspart/anyfs-lspart.exe" )
for rel in "${EXTRA_EXES[@]}"; do
    src="$BUILDDIR/$rel"
    if [[ -f "$src" ]]; then
        cp -L "$src" "$OUTDIR/$(basename "$rel")"
    else
        echo "warning: $rel not built — run \`ninja -C $BUILDDIR\` first" >&2
    fi
done

# Strip everything for size. liblkl.dll alone drops from ~220 MiB to ~15 MiB.
for f in "$OUTDIR"/*.exe "$OUTDIR"/*.dll; do
    "$STRIP" "$f"
done

# Tarball
cd "$(dirname "$OUTDIR")"
rm -f "$ARCHIVE"
tar czf "$ARCHIVE" "$(basename "$OUTDIR")"
SIZE="$(du -h "$ARCHIVE" | cut -f1)"

echo "=== Created: $(pwd)/$ARCHIVE ($SIZE) ==="
echo ""
echo "Contents:"
ls -1 "$OUTDIR" | sed 's/^/  /'
echo ""
echo "Usage on Windows (PowerShell or cmd):"
echo "  anyfs-lspart.exe <disk.img>                    # list partitions (use PATH col)"
echo "  anyfs-ksmbd.exe  <disk.img> --share disk0/p1   # serve via SMB3"
echo "  anyfs-nfsd.exe   <disk.img> --share disk0/p1   # serve via NFSv4"
