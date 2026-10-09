#!/usr/bin/env bash
# Check out the pinned drivelist fork next to this repository, where
# electron-demo's `drivelist: file:../../../../drivelist-anyfs` dependency
# expects it, and build its JavaScript (js/index.js, the package entry).
# The native drivelist.node is built separately per platform:
#   Linux:   <dir>/scripts/build-linux-electron.sh   (zig, glibc 2.25)
#   Windows: <dir>/scripts/build-win64-mingw.sh      (mingw cross build)
#   macOS:   node-gyp --runtime=electron on a Mac (Disk Arbitration)
#
# Usage: fetch-drivelist.sh [<dir>]   (default: <repo>/../drivelist-anyfs)
set -euo pipefail

DRIVELIST_REPO=https://github.com/xdqi/drivelist-anyfs
DRIVELIST_COMMIT=eac3c6dff6dca9a34ff35f18791fae5c1dc5f247

repo_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
dir="${1:-$(dirname "$repo_root")/drivelist-anyfs}"

if [[ ! -d "$dir/.git" ]]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$DRIVELIST_REPO"
fi
git -C "$dir" fetch -q --depth=1 origin "$DRIVELIST_COMMIT"
git -C "$dir" checkout -q --detach "$DRIVELIST_COMMIT"
(cd "$dir" && npm ci --ignore-scripts --no-audit --no-fund && npm run build-ts)
echo "fetch-drivelist: $DRIVELIST_REPO@$DRIVELIST_COMMIT in $dir"
