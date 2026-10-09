#!/usr/bin/env bash
# Cross-build anyfs_native.node for win64. The artifact stays in
# $package/build-win64/; collect-native.sh win32 picks it up together with
# its DLL closure (liblkl.dll, libanyfs-qemublk.dll, glib, ...), which
# package.sh stages next to it in resources/native/.
#
# No node.exe shim is needed anymore — build-win64.sh links with
# ld.lld --delayload=node.exe and binding.cc registers a delay-load
# hook that routes napi_* resolution through GetModuleHandle(NULL)
# (the host EXE).
set -euo pipefail

cd "$(dirname "$0")/../../../packages/anyfs-native"

echo ">>> Building win64 anyfs_native.node"
bash scripts/build-win64.sh

echo "OK — win64 artifact at $(pwd)/build-win64/"
ls -lh build-win64/anyfs_native.node
