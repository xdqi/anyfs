#!/usr/bin/env bash
# On a Mac: ad-hoc sign a macOS package from package.sh and write the
# release zip. electron-packager rewrote the app's Info.plist (and ran on
# Linux), so Electron's own signature no longer matches; Apple Silicon
# refuses to run code without a valid signature. Ad hoc means no
# certificate: Gatekeeper still treats the download as unidentified.
#
# Signs every Mach-O in Contents/Resources/native/ first (codesign --deep
# only descends into the standard code locations), then the bundle.
#
# Usage: sign-macos.sh <anyfs-electron-...-macos-<arch>.unsigned.tar.gz> <out-dir>
# Output: <out-dir>/<name>/ (signed app), <name>.zip and <name>.zip.sha256
set -euo pipefail

src="${1:?usage: sign-macos.sh <name.unsigned.tar.gz> <out-dir>}"
out="${2:?usage: sign-macos.sh <name.unsigned.tar.gz> <out-dir>}"
command -v codesign >/dev/null || { echo "sign-macos: needs macOS codesign" >&2; exit 1; }

name="$(basename "$src" .unsigned.tar.gz)"
mkdir -p "$out"
rm -rf "${out:?}/$name"
tar -xzf "$src" -C "$out"
app="$out/$name/anyfs-demo.app"
[[ -d "$app" ]] || { echo "sign-macos: no $app in $src" >&2; exit 1; }

for f in "$app/Contents/Resources/native/"*; do
    codesign --force --sign - "$f"
done
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

cd "$out"
rm -f "$name.zip"
ditto -c -k --keepParent "$name" "$name.zip"
shasum -a 256 "$name.zip" > "$name.zip.sha256"
echo "sign-macos: $out/$name.zip"
cat "$name.zip.sha256"
