#!/usr/bin/env bash
# Generate a minimal whole-disk ext4 image for `smoke.node.mjs single`.
# mkfs.ext4 -d populates the fs from a directory without root.
# Usage: make-single-image.sh <out.img>
set -euo pipefail
out="${1:?usage: make-single-image.sh <out.img>}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "hello from the anyfs smoke fixture" > "$tmp/hello.txt"
mkdir -p "$tmp/subdir"
head -c 1048576 /dev/urandom > "$tmp/subdir/random-1mib.bin"

mkdir -p "$(dirname "$out")"
rm -f "$out"
truncate -s 64M "$out"
mkfs_ext4="$(command -v mkfs.ext4 || command -v mke2fs || true)"
[[ -n "$mkfs_ext4" ]] || { echo "mkfs.ext4 / mke2fs not found; install e2fsprogs" >&2; exit 1; }
"$mkfs_ext4" -t ext4 -q -F -d "$tmp" "$out"
echo "wrote $out"
