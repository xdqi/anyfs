#!/usr/bin/env bash
# Install the pinned zig (toolchains.zig_version in build.config.toml) where
# scripts/lib/config.sh expects it: toolchains.zig, default
# ~/zig-<version>/zig. Idempotent; fails if a different zig is already there.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

ver="$ANYFS_TOOLCHAINS_ZIG_VERSION"
zig="$ANYFS_TOOLCHAINS_ZIG"
dest="$(dirname "$zig")"

if [[ -x "$zig" ]]; then
    have="$("$zig" version)"
    if [[ "$have" != "$ver" ]]; then
        echo "Error: $zig is zig $have, build.config.toml pins $ver" >&2
        exit 1
    fi
    echo "zig $ver already installed at $dest"
    exit 0
fi
if [[ "$(basename "$zig")" != zig || -e "$dest" ]]; then
    echo "Error: won't install into $dest (toolchains.zig = $zig)" >&2
    exit 1
fi

mkdir -p "$(dirname "$dest")"
tmp="$(mktemp -d "$(dirname "$dest")/.zig-fetch.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
url="https://ziglang.org/download/$ver/zig-x86_64-linux-$ver.tar.xz"
echo ">>> fetch $url"
curl -fL --retry 3 -o "$tmp/zig.tar.xz" "$url"
echo "$ANYFS_TOOLCHAINS_ZIG_SHA256  $tmp/zig.tar.xz" | sha256sum --check --quiet -
tar -xf "$tmp/zig.tar.xz" -C "$tmp"
mv "$tmp/zig-x86_64-linux-$ver" "$dest"
echo "zig $("$dest/zig" version) installed at $dest"
# Point .toolchain/zig at the new install (config.sh does this on load).
anyfs_load_config
