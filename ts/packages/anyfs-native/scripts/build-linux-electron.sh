#!/usr/bin/env bash
# Build anyfs_native.node (linux x64) against Electron's vendored node headers.
#
# NOTE: this is NOT actually required for the addon to load in Electron.
# anyfs_native is a PURE N-API / node-addon-api module (binding.cc includes
# <napi.h>, no v8::/node:: calls), so it is ABI-stable across NODE_MODULE_VERSION:
# a .node built with plain `node-gyp rebuild` against the host node (NMV 137)
# loads and runs fine inside Electron 42 (NMV 146) because both expose napi 10.
# Verified by experiment 2026-05-31 (require + kernelInit both succeed). The
# earlier belief that a host-ABI .node "fails to load inside Electron" was wrong;
# the F11 failure was simply a MISSING build/Release/anyfs_native.node file.
#
# So `pnpm --filter @anyfs/native build` (host-targeted node-gyp rebuild) is
# sufficient for the Electron demo too. This script is kept as a conventional
# electron-targeted builder (mirrors drivelist's); use whichever you like — the
# important thing is that the .node FILE EXISTS at build/Release/.
# (--runtime=electron would only matter for a raw-V8, non-N-API addon.)
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
