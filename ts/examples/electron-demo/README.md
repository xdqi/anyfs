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

# dev mode (vite HMR + electron)
pnpm --filter electron-demo dev

# production mode (build vite-demo, then run electron against built dist/)
pnpm --filter electron-demo start
```

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
