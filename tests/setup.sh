#!/bin/bash
# Generate test disk images for anyfs-reader test suite.
# Images are created in tests/images/ and git-ignored.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p images

# ── ext4.img (32 MiB, whole-disk ext4, ro-compatible) ────────────────
EXT4_IMG="images/ext4.img"
if [ ! -f "$EXT4_IMG" ]; then
    echo "Creating $EXT4_IMG ..."
    truncate -s 32M "$EXT4_IMG"
    mkfs.ext4 -q -F "$EXT4_IMG"
    echo "  done ($(du -h "$EXT4_IMG" | cut -f1))"
else
    echo "$EXT4_IMG already exists ($(du -h "$EXT4_IMG" | cut -f1))"
fi

# ── single.img — alias for vite-demo debug pages (/disks/single.img) ──
SINGLE_IMG="images/single.img"
if [ ! -f "$SINGLE_IMG" ]; then
    echo "Creating $SINGLE_IMG (copy of ext4.img) ..."
    cp "$EXT4_IMG" "$SINGLE_IMG"
else
    echo "$SINGLE_IMG already exists ($(du -h "$SINGLE_IMG" | cut -f1))"
fi

echo "Test images ready."
echo ""
echo "For vite-demo debug pages, symlink public/disks → tests/images:"
echo "  cd ts/examples/vite-demo/public && ln -sfn ../../../../tests/images disks"
