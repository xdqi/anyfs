#!/bin/bash
# Build QEMU static libraries for anyfs-reader
# Usage: ./build_qemu.sh [qemu_source_dir]
#
# This builds a minimal set of QEMU static libraries needed by the
# QEMU block backend. Only format drivers (qcow2, vmdk, vdi, etc.)
# and local file access are included.

set -e

QEMU_SRC="${1:-$HOME/qemu}"
BUILD_DIR="$QEMU_SRC/build-anyfs2"

if [ ! -f "$QEMU_SRC/configure" ]; then
    echo "Error: $QEMU_SRC/configure not found"
    echo "Usage: $0 [path-to-qemu-source]"
    exit 1
fi

echo "=== Configuring QEMU (minimal block-only) ==="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

"$QEMU_SRC/configure" \
    --disable-system --disable-user --enable-tools \
    --disable-guest-agent --disable-docs \
    --disable-gtk --disable-sdl --disable-opengl --disable-vnc --disable-spice \
    --disable-gnutls --disable-blkio --disable-numa \
    --disable-cap-ng --disable-seccomp --disable-libssh --disable-curl \
    --disable-rbd --disable-glusterfs --disable-vde \
    --disable-nettle --disable-gcrypt --disable-smartcard \
    --disable-usb-redir --disable-libudev --disable-fuse \
    --disable-libiscsi --disable-libnfs \
    --target-list=

echo ""
echo "=== Building static libraries ==="
ninja libblock.a libqemuutil.a libio.a libqom.a libauthz.a libcrypto.a libevent-loop-base.a

echo ""
echo "=== Building qemu-img (optional, for testing) ==="
ninja qemu-img

echo ""
echo "=== Done ==="
echo "Libraries in: $BUILD_DIR/"
ls -la "$BUILD_DIR"/*.a
echo ""
echo "To build anyfs-reader with QEMU support:"
echo "  meson setup builddir \\"
echo "    -Dlkl_root=~/linux/tools/lkl \\"
echo "    -Dqemu_root=$QEMU_SRC \\"
echo "    -Dqemu_build=$BUILD_DIR \\"
echo "    -Denable_qemu=true"
