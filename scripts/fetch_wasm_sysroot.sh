#!/usr/bin/env bash
# Download the prebuilt wasm sysroot (published by .github/workflows/
# wasm-sysroot.yml) into <repo>/.toolchain/wasm-sysroot/. Pin: bump
# WASM_SYSROOT_TAG when a new release is cut. scripts/lib/config.sh
# auto-prefers this location unless paths.wasm_sysroot is set explicitly.
# See docs/wasm-sysroot.md.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# r2: libblkid's crc32c renamed (r1 predates it; ext4 probed as unknown).
WASM_SYSROOT_TAG="${WASM_SYSROOT_TAG:-wasm-sysroot-r2}"
dest="$root/.toolchain/wasm-sysroot"
manifest="$root/scripts/lib/wasm_sysroot.manifest"
# Records which release $dest holds, so bumping the pin replaces an
# older sysroot instead of reusing it just because its libs are present.
stamp="$dest/.release-tag"
complete() {
    while IFS= read -r lib; do
        case "$lib" in ''|'#'*) continue ;; esac
        [[ -f "$dest/lib/$lib" ]] || return 1
    done < "$manifest"
}
if complete && [[ "$(cat "$stamp" 2>/dev/null)" == "$WASM_SYSROOT_TAG" ]]; then
    echo "wasm sysroot $WASM_SYSROOT_TAG already present: $dest"; exit 0
fi
rm -rf "$dest"
mkdir -p "$dest"
url="https://github.com/xdqi/anyfs/releases/download/$WASM_SYSROOT_TAG/wasm-sysroot-linux.tar.xz"
echo "fetching $url"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl -fsSL --retry 3 -o "$tmp" "$url" || {
    echo "download failed (does release tag '$WASM_SYSROOT_TAG' exist?)" >&2
    exit 1
}
tar -xJf "$tmp" -C "$dest"
complete || { echo "fetched sysroot fails the manifest check" >&2; exit 1; }
# Relocate pkg-config metadata: the tarball bakes the publisher's
# build-time prefix (the CI runner's out-sysroot dir) into every .pc,
# so -I/-L would point at a path that doesn't exist here and
# consumers (QEMU's meson glib probe) fail with misleading errors. Rewrite
# each .pc's recorded prefix — including absolute libdir/includedir
# occurrences — to this checkout's install path.
for pc in "$dest"/lib/pkgconfig/*.pc; do
    [[ -f "$pc" ]] || continue
    old_prefix="$(sed -n 's/^prefix=//p' "$pc" | head -1)"
    [[ -n "$old_prefix" && "$old_prefix" != "$dest" ]] || continue
    sed -i "s|$old_prefix|$dest|g" "$pc"
done
echo "relocated $(ls "$dest"/lib/pkgconfig/*.pc 2>/dev/null | wc -l) .pc files to $dest"
echo "$WASM_SYSROOT_TAG" > "$stamp"
echo "OK: $dest ($WASM_SYSROOT_TAG)"
