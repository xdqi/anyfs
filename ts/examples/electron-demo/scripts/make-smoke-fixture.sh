#!/usr/bin/env bash
# Build the packaged-app smoke fixture and its expected results:
#
#   <dir>/smoke.qcow2     GPT disk from core/test/make-parts-image.sh (BIOS
#                         boot, vfat "ESP", ext4 "fixroot" with metadata_csum),
#                         plus a 3 MiB pseudo-random payload.bin on fixroot,
#                         converted to qcow2 so the QEMU block layer is in the
#                         path, not only the raw backend
#   <dir>/expected.json   what check-smoke.mjs asserts
#
# Needs sfdisk, mkfs.vfat, mkfs.ext4, debugfs and qemu-img; no root.
# Usage: make-smoke-fixture.sh <dir>
set -euo pipefail

dir="${1:?usage: make-smoke-fixture.sh <dir>}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
ts_root="$(cd "$script_dir/../../.." && pwd)"
command -v debugfs >/dev/null || { echo "debugfs not found; install e2fsprogs" >&2; exit 1; }
command -v qemu-img >/dev/null || { echo "qemu-img not found; install qemu-utils" >&2; exit 1; }

mkdir -p "$dir"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

bash "$ts_root/packages/core/test/make-parts-image.sh" "$tmp/disk.img"

# Fixed seed, so the payload (and its hash) is the same on every run.
openssl enc -aes-256-ctr -pass pass:anyfs-smoke -nosalt -pbkdf2 \
    < <(head -c $((3 * 1024 * 1024)) /dev/zero) > "$tmp/payload.bin" 2>/dev/null
# fixroot is partition 3: start sector 71680, 65536 sectors.
dd if="$tmp/disk.img" of="$tmp/root.img" bs=512 skip=71680 count=65536 status=none
debugfs -w -R "write $tmp/payload.bin payload.bin" "$tmp/root.img" >/dev/null 2>&1
debugfs -R "stat payload.bin" "$tmp/root.img" 2>/dev/null | grep -q 'Size: 3145728' ||
    { echo "make-smoke-fixture: payload.bin not written" >&2; exit 1; }
dd if="$tmp/root.img" of="$tmp/disk.img" bs=512 seek=71680 conv=notrunc status=none

qemu-img convert -f raw -O qcow2 "$tmp/disk.img" "$dir/smoke.qcow2"

sha="$(sha256sum "$tmp/payload.bin" | cut -d' ' -f1)"
cat > "$dir/expected.json" <<JSON
{
  "image": "smoke.qcow2",
  "partitions": [
    { "fstype": "vfat", "label": "ESP" },
    { "fstype": "ext4", "label": "fixroot" }
  ],
  "part": "fixroot",
  "entries": ["hello.txt", "payload.bin"],
  "read": "payload.bin",
  "size": 3145728,
  "sha256": "$sha"
}
JSON
echo "make-smoke-fixture: $dir/smoke.qcow2 (payload sha256 $sha)"
