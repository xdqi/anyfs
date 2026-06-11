#!/usr/bin/env bash
# Download the prebuilt patched wasm-ld (published by xdqi/llvm-wasm's
# wasm-ld-release.yml workflow) into <repo>/.toolchain/wasm-ld/. Pin: bump
# WASM_LD_TAG when a new release is cut. scripts/lib/config.sh auto-prefers
# this location unless toolchains.wasm_ld is set explicitly.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
WASM_LD_TAG="${WASM_LD_TAG:-wasm-ld-18.1.2-anyfs-r1}"
dest="$root/.toolchain/wasm-ld"
if [[ -x "$dest/wasm-ld" ]] && "$dest/wasm-ld" --version 2>/dev/null | grep -q 'LLD 18'; then
    echo "wasm-ld already present: $dest/wasm-ld"; exit 0
fi
mkdir -p "$dest"
url="https://github.com/xdqi/llvm-wasm/releases/download/$WASM_LD_TAG/wasm-ld-linux-amd64.tar.xz"
echo "fetching $url"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl -fsSL --retry 3 -o "$tmp" "$url" || {
    echo "download failed (does release tag '$WASM_LD_TAG' exist?)" >&2
    exit 1
}
tar -xJf "$tmp" -C "$dest"
[[ -f "$dest/wasm-ld" ]] || { echo "archive did not contain wasm-ld" >&2; exit 1; }
chmod +x "$dest/wasm-ld"
"$dest/wasm-ld" --version | grep -q 'LLD 18' || {
    echo "fetched wasm-ld does not report 'LLD 18':" >&2
    "$dest/wasm-ld" --version >&2 || true
    exit 1
}
echo "OK: $dest/wasm-ld ($("$dest/wasm-ld" --version | head -1))"
