#!/usr/bin/env bash
# Stage the macOS native addon into a packaged electron-demo app.
#
# Usage: stage-native-macos.sh <packaged-dir | App.app> <arm64|x86_64> [<native-dir>]
#
# <packaged-dir> is electron-packager's output directory for one darwin arch
# (e.g. out/anyfs-demo-darwin-arm64, holding anyfs-demo.app); an .app path
# works too. <native-dir> (or $ANYFS_NATIVE_DIR) holds the addon, e.g. a
# downloaded CI artifact; default packages/anyfs-native/build-macos-<arch>/
# (scripts/build-macos.sh). Copies from it:
#   anyfs_native.node     -> <App>.app/Contents/Resources/native/
#   liblkl-kernel.dylib   -> <App>.app/Contents/Resources/native/
# native-loader.ts loads join(process.resourcesPath, 'native',
# 'anyfs_native.node'); process.resourcesPath is Contents/Resources on macOS.
# The .node finds the kernel dylib through its LC_RPATH @loader_path, so the
# two must stay side by side; no DYLD_* variable is involved. No drivelist on
# macOS: main.ts treats it as optional.
#
# Checks: the app's executable and the addon are the same arch, and the
# staged files pass scripts/macho/check_macho.sh. Needs LLVM 19 or 20's
# llvm-objdump, llvm-otool and llvm-nm (scripts/macho/llvm_tools.sh).
# Signing is the caller's: electron-packager on Linux leaves the bundle's
# signature broken, so on the Mac run `codesign --force --deep --sign -
# <App>.app` before the first launch (docs/macos.md).
set -euo pipefail

usage="usage: stage-native-macos.sh <packaged-dir|App.app> <arm64|x86_64> [<native-dir>]"
target="${1:?$usage}"
arch="${2:?$usage}"
ts_root="$(cd "$(dirname "$0")/../../.." && pwd)"
repo_root="$(cd "$ts_root/.." && pwd)"
src="${3:-${ANYFS_NATIVE_DIR:-$ts_root/packages/anyfs-native/build-macos-$arch}}"
# shellcheck source=../../../../scripts/macho/llvm_tools.sh
source "$repo_root/scripts/macho/llvm_tools.sh"
objdump="$(llvm_tool llvm-objdump)"

case "$arch" in
    arm64)  cputype=ARM64 ;;
    x86_64) cputype=X86_64 ;;
    *) echo "stage-native-macos: arch must be arm64 or x86_64" >&2; exit 1 ;;
esac

if [[ "$target" == *.app ]]; then
    app="$target"
else
    shopt -s nullglob
    apps=("$target"/*.app)
    [[ ${#apps[@]} -eq 1 ]] || { echo "stage-native-macos: expected one .app in $target" >&2; exit 1; }
    app="${apps[0]}"
fi
[[ -d "$app/Contents/Resources" ]] || { echo "stage-native-macos: $app is not an app bundle" >&2; exit 1; }

for f in anyfs_native.node liblkl-kernel.dylib; do
    [[ -f "$src/$f" ]] || {
        echo "stage-native-macos: missing $src/$f (run packages/anyfs-native/scripts/build-macos.sh --arch=$arch)" >&2
        exit 1
    }
done

exe="$app/Contents/MacOS/$(basename "$app" .app)"
[[ -f "$exe" ]] || { echo "stage-native-macos: no executable $exe" >&2; exit 1; }
exe_types="$("$objdump" --macho --private-headers "$exe" | awk '/^ *(0x)?MH_MAGIC/ {print $2}' | sort -u)"
[[ "$exe_types" == "$cputype" ]] || {
    echo "stage-native-macos: $exe is {${exe_types//$'\n'/ }}, the addon is $cputype" >&2
    exit 1
}

native_dir="$app/Contents/Resources/native"
mkdir -p "$native_dir"
cp -- "$src/anyfs_native.node" "$src/liblkl-kernel.dylib" "$native_dir/"

"$repo_root/scripts/macho/check_macho.sh" --arch="$arch" \
    --dylib=@rpath/liblkl-kernel.dylib --rpath=@loader_path \
    --allow-undefined='^_(napi|node_api)_' "$native_dir/anyfs_native.node"
"$repo_root/scripts/macho/check_macho.sh" --arch="$arch" "$native_dir/liblkl-kernel.dylib"

echo "stage-native-macos: staged into $native_dir"
ls -l "$native_dir"
