#!/usr/bin/env bash
# Make the runtime-test fixtures (macOS tests, and the CLI device tests on
# every OS) with the same partitions, filesystem ids, labels and file
# contents on every run, so their reference data can live in
# tests/macos/reference/. The image bytes themselves may differ between runs
# and mkfs versions (ext4 metadata such as inode times); nothing compares
# them.
#
# Usage: make-fixtures.sh <outdir>
#
#   parts.img       70 MiB GPT disk shaped like an Ubuntu cloud image (the
#                   layout of ts/packages/core/test/make-parts-image.sh):
#                     #1 BIOS boot, 1 MiB, no filesystem
#                     #2 EFI System, 33 MiB, vfat "ESP", volume id 2A2A-2A2A
#                     #3 Linux root, 32 MiB, ext4 "fixroot", metadata_csum,
#                        fixed UUID, holding hello.txt and payload.bin
#                        (3 MiB from a fixed seed, so reads cross many
#                        sectors and I/O chunks)
#   parts-zlib.dmg  parts.img as a UDZO (zlib) disk image
#   parts-bz2.dmg   parts.img as a UDBZ (bzip2) disk image
#
# metadata_csum matters: libblkid verifies the ext4 superblock checksum with
# crc32c, which a crc32c symbol clash with QEMU breaks. Built without root.
# Needs sfdisk, mkfs.vfat (dosfstools), mkfs.ext4 (e2fsprogs) and python3.
set -euo pipefail

out="${1:?usage: make-fixtures.sh <outdir>}"
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
for t in sfdisk mkfs.vfat mkfs.ext4 python3; do
    command -v "$t" > /dev/null || [[ -x /usr/sbin/$t || -x /sbin/$t ]] \
        || { echo "make-fixtures: $t not found" >&2; exit 1; }
done
export PATH="$PATH:/usr/sbin:/sbin"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out" "$tmp/root"
echo "hello from the anyfs parts fixture" > "$tmp/root/hello.txt"
python3 - "$tmp/root/payload.bin" <<'PY'
import random, sys
open(sys.argv[1], "wb").write(random.Random(20261009).randbytes(3 << 20))
PY
touch -d '2026-01-01 00:00:00 UTC' "$tmp/root/hello.txt" "$tmp/root/payload.bin" "$tmp/root"

truncate -s 33M "$tmp/esp.img"
mkfs.vfat -n ESP -i 2A2A2A2A "$tmp/esp.img" > /dev/null
truncate -s 32M "$tmp/root.img"
E2FSPROGS_FAKE_TIME=1767225600 mkfs.ext4 -q -F -O metadata_csum -L fixroot \
    -U 5c502e71-caaf-4fb7-810e-7578df86ca8e \
    -E hash_seed=0e7cc2c3-2a7e-4f2e-9c3a-6f1a2b3c4d5e -d "$tmp/root" "$tmp/root.img"

img="$out/parts.img"
rm -f "$img"
truncate -s 70M "$img"
sfdisk -q "$img" <<'EOF'
label: gpt
label-id: 6E1C5B1A-9D3F-4C11-8A5B-0F2D3C4B5A69
start=2048,  size=2048,  type=21686148-6449-6E6F-744E-656564454649, uuid=1B2C3D4E-0001-4000-8000-000000000001
start=4096,  size=67584, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=1B2C3D4E-0002-4000-8000-000000000002
start=71680, size=65536, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709, uuid=1B2C3D4E-0003-4000-8000-000000000003
EOF
dd if="$tmp/esp.img" of="$img" bs=1M seek=2 conv=notrunc status=none
dd if="$tmp/root.img" of="$img" bs=1M seek=35 conv=notrunc status=none

python3 "$repo/tests/make_dmg_image.py" --codec zlib "$img" "$out/parts-zlib.dmg"
python3 "$repo/tests/make_dmg_image.py" --codec bz2 "$img" "$out/parts-bz2.dmg"
echo "make-fixtures: $out/{parts.img,parts-zlib.dmg,parts-bz2.dmg}"
