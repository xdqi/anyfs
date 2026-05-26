#!/usr/bin/env bash
# Stage native .node files into a packaged Electron app's resources/native/.
#
# Called after electron-packager so the staged tree contains:
#   resources/native/anyfs_native.node
#   resources/native/drivelist.node
#
# native-loader.ts in the bundled main process resolves these paths via
# join(process.resourcesPath, 'native', ...).
#
# Usage: stage-native.sh <packaged-dir> [strip|no-strip]

set -euo pipefail

target="${1:?usage: stage-native.sh <packaged-dir> [strip|no-strip]}"
strip_mode="${2:-strip}"

ts_root="$(cd "$(dirname "$0")/../../.." && pwd)"

native_dir="$target/resources/native"
mkdir -p "$native_dir"

anyfs_src="$ts_root/packages/anyfs-native/build/Release/anyfs_native.node"
drivelist_src="$ts_root/../../drivelist-anyfs/build/Release/drivelist.node"

[ -f "$anyfs_src" ] || { echo "stage-native: missing $anyfs_src" >&2; exit 1; }
[ -f "$drivelist_src" ] || { echo "stage-native: missing $drivelist_src" >&2; exit 1; }

cp -- "$anyfs_src" "$native_dir/anyfs_native.node"
cp -- "$drivelist_src" "$native_dir/drivelist.node"

if [ "$strip_mode" = strip ]; then
    strip --strip-debug "$native_dir/anyfs_native.node"
    strip --strip-debug "$native_dir/drivelist.node"
fi

echo "stage-native: staged into $native_dir"
ls -lh "$native_dir"
