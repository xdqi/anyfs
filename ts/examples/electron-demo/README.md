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

# REQUIRED for native mode: the native addon must be BUILT (the .node is a
# gitignored local artifact). If build/Release/anyfs_native.node is missing the
# app logs "[anyfs-native] addon not loadable" and falls back to wasm.
bash packages/anyfs-native/scripts/build-linux-electron.sh   # or: pnpm --filter @anyfs/native build

# dev mode (vite HMR + electron)
pnpm --filter electron-demo dev

# production mode (build vite-demo, then run electron against built dist/)
pnpm --filter electron-demo start
```

## Native addon (IMPORTANT)

`anyfs_native.node` just needs to **exist** at `build/Release/` — it's a
gitignored artifact, so a fresh checkout has none and native mode silently falls
back to wasm until you build it.

`anyfs_native` is a **pure N-API / node-addon-api** module, so it is ABI-stable
across NODE_MODULE_VERSION: a `.node` built for the host node (v24 → NMV 137)
loads and runs fine inside Electron (NMV 146) because both expose **napi 10**.
N-API validates the napi version, not NODE_MODULE_VERSION. (NMV tracks V8, not
the node version string — Electron reskins node v24 with a newer V8, hence the
different number; it does not matter here.) So **plain
`pnpm --filter @anyfs/native build` (`node-gyp rebuild`, host-targeted) is
sufficient**.

`scripts/build-linux-electron.sh` (`node-gyp --runtime=electron`) is provided as
a conventional builder and works too, but the `--runtime=electron` part is NOT
required for this addon to load — it would only matter for a raw-V8 (non-N-API)
addon, which this is not. `drivelist.node` ships its own build script.

```sh
# from ts/ — either of these produces build/Release/anyfs_native.node:
pnpm --filter @anyfs/native build
bash packages/anyfs-native/scripts/build-linux-electron.sh
```

Both dev (`native-loader.ts` resolves `build/Release/`) and the Linux package
(`scripts/stage-native.sh` copies from the same dir) consume it. The win64
package uses the cross-built `build-win64/anyfs_native.node` (see
`scripts/build-win64.sh`); it links Electron's `win-x64/node.lib` for the mingw
cross-link, not for ABI reasons.

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
