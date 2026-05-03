#!/bin/bash
# Build libanyfs-qemu.so from QEMU block layer static libraries
# Must be run after QEMU is configured and built with -fPIC -fno-pie
#
# Prerequisites:
#   cd ~/qemu && mkdir build-anyfs-shared
#   ../configure --disable-system --disable-user --enable-tools \
#     --disable-guest-agent --disable-docs --disable-gtk --disable-sdl \
#     --disable-opengl --disable-vnc --disable-spice --disable-gnutls \
#     --disable-blkio --disable-numa --disable-cap-ng --disable-seccomp \
#     --disable-libssh --disable-curl --disable-rbd --disable-glusterfs \
#     --disable-vde --disable-nettle --disable-gcrypt --disable-smartcard \
#     --disable-usb-redir --disable-libudev --disable-fuse \
#     --disable-libiscsi --disable-libnfs --target-list= \
#     --extra-cflags="-fPIC"
#   meson configure -Db_pie=false
#   ninja libblock.a libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a
#
# IMPORTANT: util/fdmon-poll.c must have 'static' removed from __thread
# declarations, otherwise R_X86_64_TPOFF32 relocations prevent shared linking.

set -e

QEMU_BUILD="${1:-.}"

cd "$QEMU_BUILD"

# Verify libs exist
for lib in libblock.a libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a; do
    if [ ! -f "$lib" ]; then
        echo "ERROR: $lib not found in $QEMU_BUILD" >&2
        exit 1
    fi
done

# Link shared library
# - libblock.a: whole-archive (needs format driver constructors)
# - others: in --start-group to resolve circular dependencies
gcc -shared -o libanyfs-qemu.so \
    -Wl,--whole-archive libblock.a \
    -Wl,--no-whole-archive \
    -Wl,--start-group \
    libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a \
    -Wl,--end-group \
    $(pkg-config --libs glib-2.0 gthread-2.0 zlib pixman-1 libzstd) \
    $(pkg-config --libs liburing 2>/dev/null || true) \
    -laio -lbz2 -lm

echo "Built: $(ls -lh libanyfs-qemu.so | awk '{print $5, $NF}')"
