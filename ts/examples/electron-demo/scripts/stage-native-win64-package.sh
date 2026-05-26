#!/usr/bin/env bash
# Stage win64 native artifacts into a packaged Electron app's resources/native/.
#
# Both .node files were linked with ld.lld --delayload=node.exe, so neither
# imports node.exe directly — their delay-load hooks redirect to the host
# EXE via GetModuleHandle(NULL). No node.exe shim DLL needed.
# (See [[mingw-delayload-lld]] in claude memory for background.)
#
# Transitive DLLs (liblkl.dll, libanyfs-qemublk.dll, glib, …) still need
# to be next to anyfs-demo.exe — that's handled by copy-win64-dlls.sh.
#
# Usage: stage-native-win64-package.sh <packaged-dir>
set -euo pipefail

target="${1:?usage: stage-native-win64-package.sh <packaged-dir>}"
ts_root="$(cd "$(dirname "$0")/../../.." && pwd)"

native_dir="$target/resources/native"
mkdir -p "$native_dir"

anyfs_node="$ts_root/packages/anyfs-native/build-win64/anyfs_native.node"
drivelist_node="$ts_root/../../drivelist-anyfs/build-win64/drivelist.node"

[[ -f "$anyfs_node" ]] || { echo "missing: $anyfs_node" >&2; exit 1; }
[[ -f "$drivelist_node" ]] || { echo "missing: $drivelist_node" >&2; exit 1; }

cp -- "$anyfs_node" "$native_dir/anyfs_native.node"
cp -- "$drivelist_node" "$native_dir/drivelist.node"

echo "stage-native-win64: staged into $native_dir"
ls -lh "$native_dir"
