#!/bin/bash
# Package the anyfs-reader command-line tools for macOS.
#
# Usage: ./scripts/package_macos.sh --arch=arm64|x86_64 [--out=DIR]
#
# Takes anyfs-ksmbd, anyfs-nfsd and anyfs-lspart from build-anyfs-macos-<arch>
# (scripts/build_anyfs.sh --targets=macos-<arch> --components=core,server)
# and the kernel, build/macos/<arch>/liblkl-kernel.dylib, and writes
#   <out>/anyfs-reader-<version>-macos-<arch>.tar.gz
#     bin/anyfs-ksmbd bin/anyfs-nfsd bin/anyfs-lspart [bin/anyfs-fuse]
#     lib/liblkl-kernel.dylib
# anyfs-fuse is included when it was built (build_macos_sysroot.sh
# --only=macfuse): it loads /usr/local/lib/libfuse3.4.dylib, so it runs only
# where macFUSE is installed; the other tools need nothing beyond macOS.
# Everything else is linked statically; the tools find the kernel through
# their LC_RPATH @loader_path/../lib. Every Mach-O passes
# scripts/macho/check_macho.sh before the tarball is written. Nothing is
# stripped: on arm64 that would invalidate the linker's ad-hoc signature.
# <out> defaults to build/macos; ANYFS_VERSION overrides the date version.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

arch="" out="$SRC_DIR/build/macos"
for a in "$@"; do
    case "$a" in
        --arch=*) arch="${a#--arch=}" ;;
        --out=*)  out="${a#--out=}" ;;
        -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 1 ;;
    esac
done
case "$arch" in
    arm64|x86_64) ;;
    *) echo "--arch=arm64|x86_64 is required" >&2; exit 1 ;;
esac

VERSION="${ANYFS_VERSION:-$(date +%Y%m%d)}"
NAME="anyfs-reader-$VERSION-macos-$arch"
BUILD="$SRC_DIR/build-anyfs-macos-$arch"
KERNEL="$SRC_DIR/build/macos/$arch/liblkl-kernel.dylib"
TOOLS=(anyfs-ksmbd anyfs-nfsd anyfs-lspart)

for f in "$BUILD/anyfs-ksmbd" "$BUILD/anyfs-nfsd" "$BUILD/src/lspart/anyfs-lspart" "$KERNEL"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (scripts/build_anyfs.sh --targets=macos-$arch --components=core,server)" >&2; exit 1; }
done

mkdir -p "$out"
staging="$(mktemp -d "$out/.package.XXXXXX")"
trap 'rm -rf "$staging"' EXIT
pkg="$staging/$NAME"
mkdir -p "$pkg/bin" "$pkg/lib"
cp "$BUILD/anyfs-ksmbd" "$BUILD/anyfs-nfsd" "$BUILD/src/lspart/anyfs-lspart" "$pkg/bin/"
cp "$KERNEL" "$pkg/lib/"

"$SCRIPT_DIR/macho/check_macho.sh" --arch="$arch" \
    --dylib=@rpath/liblkl-kernel.dylib --rpath=@loader_path --rpath=@loader_path/../lib \
    "${TOOLS[@]/#/$pkg/bin/}"
if [[ -f "$BUILD/anyfs-fuse" ]]; then
    cp "$BUILD/anyfs-fuse" "$pkg/bin/"
    "$SCRIPT_DIR/macho/check_macho.sh" --arch="$arch" \
        --dylib=@rpath/liblkl-kernel.dylib --dylib=/usr/local/lib/libfuse3.4.dylib \
        --rpath=@loader_path --rpath=@loader_path/../lib "$pkg/bin/anyfs-fuse"
else
    echo "note: no anyfs-fuse in $BUILD (build_macos_sysroot.sh --arch=$arch --only=macfuse)"
fi
"$SCRIPT_DIR/macho/check_macho.sh" --arch="$arch" "$pkg/lib/liblkl-kernel.dylib"

tar czf "$out/$NAME.tar.gz" -C "$staging" "$NAME"
echo "=== Package created: $out/$NAME.tar.gz ==="
tar tzvf "$out/$NAME.tar.gz"
