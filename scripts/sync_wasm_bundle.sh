#!/usr/bin/env bash
# Copy the browser wasm bundle from @anyfs/core into vite-demo's public/wasm/.
# vite-demo loads /wasm/anyfs.worker.js (App.tsx) which imports anyfs.mjs;
# the anyfs.workeronly.* files are stale and intentionally not synced.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/ts/packages/core/wasm"
dst="$root/ts/examples/vite-demo/public/wasm"

mkdir -p "$dst"
for f in anyfs.mjs anyfs.wasm anyfs.worker.js; do
    [[ -f "$src/$f" ]] || {
        echo "missing $src/$f (run scripts/build_anyfs_wasm.sh first)" >&2
        exit 1
    }
    cp --remove-destination "$src/$f" "$dst/$f"
done
echo "synced anyfs.{mjs,wasm,worker.js} -> $dst"
