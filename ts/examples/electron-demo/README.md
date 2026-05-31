# electron-demo

Electron wrapper around `vite-demo`. The renderer is the **same React app**
that `vite-demo` deploys to the web — no source duplication. This package
only adds the Electron main process + `anyfs://app/` custom protocol so the
window qualifies as a cross-origin-isolated secure context (required for
SharedArrayBuffer + the streaming-download service worker).

## Quick start

```sh
# from monorepo root
cd ts
pnpm install

# REQUIRED for native mode: build the addon against Electron's ABI (see below).
# Skip this and the app falls back to wasm, logging "[anyfs-native] addon not
# loadable" — Electron can't load a .node built against the host node's ABI.
bash packages/anyfs-native/scripts/build-linux-electron.sh

# dev mode (vite HMR + electron)
pnpm --filter electron-demo dev

# production mode (build vite-demo, then run electron against built dist/)
pnpm --filter electron-demo start
```

## Native addon ABI (IMPORTANT)

`anyfs_native.node` (and `drivelist.node`) must be compiled against **Electron's
vendored node headers**, not the host node's. Electron N embeds a libnode whose
module ABI differs from the host (host node v24 → ABI 137; Electron 42 → ABI 146).
A `.node` built with a plain `node-gyp rebuild` (the `pnpm --filter @anyfs/native
build` script, which targets the host) compiles fine but **fails to load inside
Electron** — surfacing in the demo as native-mode silently degrading to wasm.

Build the Electron-targeted addon with:

```sh
# from ts/
bash packages/anyfs-native/scripts/build-linux-electron.sh   # writes build/Release/anyfs_native.node
bash ../../drivelist-anyfs/scripts/build-linux-electron.sh    # writes build/Release/drivelist.node
```

Both dev (`native-loader.ts` resolves `build/Release/`) and the Linux package
(`scripts/stage-native.sh` copies from the same dir) consume these. The win64
package uses the cross-built `build-win64/anyfs_native.node` instead (see
`scripts/build-win64.sh`), which is already linked against Electron's `node.lib`.

## Why a custom protocol

- `file://` can't inject response headers → no COOP/COEP → no
  `SharedArrayBuffer` → wasm Worker's `Atomics.wait` fails.
- `file://` isn't a secure context → `sw-download.js` can't register.
- `anyfs://` is registered with `secure: true` privilege and served via
  `protocol.handle()`, which lets us return a real `Response` with both
  headers attached on every load.

## Layout

```
electron-demo/
├── src/
│   ├── main.ts       # Electron main: registers anyfs://, opens BrowserWindow
│   └── preload.ts    # Empty placeholder for future N-API native bridge
├── tsup.config.ts    # Compiles main+preload to dist/{main,preload}.cjs
└── package.json
```

## Native module backend (future)

`preload.ts` will expose `window.anyfsNative` via `contextBridge` once the
N-API addon lands. The renderer's `@anyfs/core` will detect the bridge at
runtime and pick native over wasm. Wasm stays as the fallback.
