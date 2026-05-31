#!/usr/bin/env bash
# Build anyfs_native.node (linux x64) against Electron's vendored node headers
# rather than the host node's. Required because Electron N's bundled libnode
# uses a different module ABI than the host node — a .node built with a plain
# `node-gyp rebuild` (host headers) compiles fine but FAILS to load inside
# Electron with an ABI/version mismatch, surfacing as "native module not found"
# in the demo. Mirrors drivelist-anyfs/scripts/build-linux-electron.sh.
# See [[native-addon-vs-electron-headers]] in claude memory for background:
#   host node v24 → module ABI 137; Electron 42 → module ABI 146.
#
# `pnpm build` (plain `node-gyp rebuild`) still exists and targets the HOST
# node — use that only for the Node-side smoke test / @anyfs/native consumers
# running under node. For the Electron demo, use THIS script.
#
# Default ELECTRON_TARGET tracks ts/examples/electron-demo's installed electron
# (resolved from its package.json); override via env when bumping electron.
#
# Output: build/Release/anyfs_native.node  (loaded by electron-demo in dev via
#         native-loader.ts, and staged into packaged apps by stage-native.sh).
set -euo pipefail

cd "$(dirname "$0")/.."

# Resolve the demo's electron version unless the caller pinned one.
if [[ -z "${ELECTRON_TARGET:-}" ]]; then
    demo_pkg="../../examples/electron-demo/node_modules/electron/package.json"
    if [[ -f "$demo_pkg" ]]; then
        ELECTRON_TARGET="$(node -p "require('$demo_pkg').version")"
    else
        ELECTRON_TARGET="42.3.0"
    fi
fi
echo ">>> Building anyfs_native.node against Electron v${ELECTRON_TARGET} headers"

# node-gyp's --runtime=electron --dist-url switches the headers tarball URL to
# electronjs.org/headers (rather than nodejs.org/dist) and writes them under
# ~/.cache/node-gyp/<ver>.
npx node-gyp install \
    --target="$ELECTRON_TARGET" \
    --dist-url=https://electronjs.org/headers \
    --runtime=electron

npx node-gyp rebuild \
    --target="$ELECTRON_TARGET" \
    --dist-url=https://electronjs.org/headers \
    --runtime=electron \
    --arch=x64

echo
echo "Built: $(file build/Release/anyfs_native.node | head -1)"
echo "Size : $(stat -c %s build/Release/anyfs_native.node) bytes"
