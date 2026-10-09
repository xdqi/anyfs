#!/usr/bin/env bash
# Package the desktop app for one platform/arch and write a release archive.
#
# Inputs, prepared by the caller:
#   dist/main.cjs            `pnpm build:main`
#   staging/renderer/        `pnpm stage:renderer` (vite-demo build incl. the
#                            hashed wasm fallback under wasm/<hash>/)
#   --native-dir             collect-native.sh output (or the CI artifact):
#                            anyfs_native.node [+ drivelist.node] [+ *.dll]
#
# Steps: electron-packager -> stage the native payload into resources/native/
# -> write resources/build-info.json -> verify-package.sh -> archive + .sha256.
#
# Output: <out>/anyfs-electron-<version>-<os>-<arch>.{tar.gz|zip} (+ .sha256)
# where os is linux|windows|macos. Linux ships a tar.gz (keeps modes and
# symlinks); Windows and macOS ship a zip. The unpacked tree stays in
# <out>/anyfs-electron-<version>-<os>-<arch>/ for smoke tests.
#
# Usage:
#   package.sh --platform=linux|win32|darwin --arch=x64|arm64
#              (--native-dir=<dir> | --no-native) [--version=<v>] [--commit=<sha>]
#              [--out=<dir>]
set -euo pipefail

platform='' arch='' native_dir='' no_native=0 version='' commit='' out=''
for a in "$@"; do
    case "$a" in
    --platform=*) platform="${a#*=}" ;;
    --arch=*) arch="${a#*=}" ;;
    --native-dir=*) native_dir="${a#*=}" ;;
    --no-native) no_native=1 ;;
    --version=*) version="${a#*=}" ;;
    --commit=*) commit="${a#*=}" ;;
    --out=*) out="${a#*=}" ;;
    *) echo "package.sh: unknown argument $a" >&2; exit 2 ;;
    esac
done

app_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$app_dir"

case "$platform" in
linux) os=linux ;;
win32) os=windows ;;
darwin) os=macos ;;
*) echo "package.sh: --platform must be linux, win32 or darwin" >&2; exit 2 ;;
esac
case "$arch" in
x64|arm64) ;;
*) echo "package.sh: --arch must be x64 or arm64" >&2; exit 2 ;;
esac
if [[ $no_native -eq 0 && -z "$native_dir" ]]; then
    echo "package.sh: pass --native-dir=<dir>, or --no-native for a wasm-only package" >&2
    exit 2
fi
commit="${commit:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"
version="${version:-sha-${commit:0:7}}"
out="$(mkdir -p "${out:-out}" && cd "${out:-out}" && pwd)"

[[ -f dist/main.cjs ]] || { echo "package.sh: dist/main.cjs missing (pnpm build:main)" >&2; exit 1; }
[[ -f staging/renderer/index.html ]] || {
    echo "package.sh: staging/renderer missing (pnpm stage:renderer)" >&2
    exit 1
}

# electron-packager writes <out>/anyfs-demo-<platform>-<arch>/. Its tmpdir
# holds a full Electron copy; keep it off a small /tmp tmpfs.
pkg_tmp="${XDG_CACHE_HOME:-$HOME/.cache}/electron-packager-tmp"
mkdir -p "$pkg_tmp"
TMPDIR="$pkg_tmp" npx electron-packager . anyfs-demo \
    --platform="$platform" --arch="$arch" --out="$out" --overwrite --prune=false \
    --extra-resource=staging/renderer \
    --ignore='^/(staging|out|src|esbuild\.main\.mjs|tsconfig\.json|README\.md|\.gitignore|scripts|node_modules)($|/)'

built="$out/anyfs-demo-$platform-$arch"
name="anyfs-electron-$version-$os-$arch"
# electron-packager has exited 0 without writing its output on CI before
# (node 24.16 + yauzl); fail here instead of at the archive step.
[[ -d "$built" ]] || { echo "package.sh: electron-packager wrote no $built" >&2; exit 1; }
rm -rf "${out:?}/$name"
mv "$built" "$out/$name"
pkg="$out/$name"

if [[ "$platform" == darwin ]]; then
    resources="$pkg/anyfs-demo.app/Contents/Resources"
else
    resources="$pkg/resources"
fi

if [[ $no_native -eq 0 ]]; then
    mkdir -p "$resources/native"
    cp -R "$native_dir"/. "$resources/native/"
fi

cat > "$resources/build-info.json" <<EOF
{
  "version": "$version",
  "commit": "$commit",
  "platform": "$platform",
  "arch": "$arch",
  "native": $([[ $no_native -eq 0 ]] && echo true || echo false)
}
EOF

verify_args=("$pkg" "$platform" "$arch")
[[ $no_native -eq 1 ]] && verify_args+=(--no-native)
bash "$app_dir/scripts/verify-package.sh" "${verify_args[@]}"

cd "$out"
case "$platform" in
linux)
    archive="$name.tar.gz"
    tar -czf "$archive" "$name"
    ;;
darwin)
    archive="$name.zip"
    rm -f "$archive"
    if command -v ditto >/dev/null; then
        ditto -c -k --keepParent "$name" "$archive"
    else
        zip -qry "$archive" "$name"
    fi
    ;;
win32)
    archive="$name.zip"
    rm -f "$archive"
    zip -qr "$archive" "$name"
    ;;
esac
if command -v sha256sum >/dev/null; then
    sha256sum "$archive" > "$archive.sha256"
else
    shasum -a 256 "$archive" > "$archive.sha256"
fi
echo "package.sh: $out/$archive"
cat "$archive.sha256"
