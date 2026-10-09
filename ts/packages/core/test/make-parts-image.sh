#!/usr/bin/env bash
# Generate a small GPT disk shaped like an Ubuntu cloud image for
# `smoke.node.mjs parts`: partition-type and libblkid regressions.
#   #1  BIOS boot        1 MiB, no filesystem
#   #2  EFI System      33 MiB, vfat  label ESP
#   #3  Linux root      32 MiB, ext4  label fixroot, metadata_csum
# metadata_csum is the point: libblkid verifies the ext4 superblock
# checksum with crc32c, which is what a crc32c symbol clash breaks.
# Built without root: each filesystem is made in its own file and copied
# into place.
# Usage: make-parts-image.sh <out.img>
set -euo pipefail
out="${1:?usage: make-parts-image.sh <out.img>}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

need() { command -v "$1" >/dev/null || { echo "$1 not found; install $2" >&2; exit 1; }; }
need sfdisk fdisk
need mkfs.vfat dosfstools
need mkfs.ext4 e2fsprogs

mkdir -p "$tmp/root"
echo "hello from the anyfs parts fixture" > "$tmp/root/hello.txt"

truncate -s 33M "$tmp/esp.img"
mkfs.vfat -n ESP "$tmp/esp.img" >/dev/null
truncate -s 32M "$tmp/root.img"
mkfs.ext4 -q -F -O metadata_csum -L fixroot -d "$tmp/root" "$tmp/root.img"

mkdir -p "$(dirname "$out")"
rm -f "$out"
truncate -s 70M "$out"
sfdisk -q "$out" <<'EOF'
label: gpt
start=2048,  size=2048,  type=21686148-6449-6E6F-744E-656564454649
start=4096,  size=67584, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
start=71680, size=65536, type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709
EOF
dd if="$tmp/esp.img" of="$out" bs=1M seek=2 conv=notrunc status=none
dd if="$tmp/root.img" of="$out" bs=1M seek=35 conv=notrunc status=none
echo "wrote $out"
