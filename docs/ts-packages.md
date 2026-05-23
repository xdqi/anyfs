# TypeScript / Browser Packages

The `ts/` workspace ships the anyfs stack as Node and browser libraries. The
wasm kernel build (LKL + QEMU block layer compiled with Emscripten) is wrapped
by a small set of `@anyfs/*` packages and consumed by two reference apps.

A live build of the browser demo is hosted at **<https://anyfs.kosaka.moe>**.

## Layout

```
ts/
├── packages/
│   ├── core/          @anyfs/core    — wasm kernel + JS bindings (browser + Node)
│   ├── react/         @anyfs/react   — React hooks/provider over @anyfs/core
│   ├── trees/         @anyfs/trees   — AnyfsFileBrowser (Chonky-based file UI)
│   └── anyfs-native/  @anyfs/native  — Node N-API addon (uses the native lib, no wasm)
├── examples/
│   ├── vite-demo/     — browser demo, deployed to anyfs.kosaka.moe
│   └── electron-demo/ — Electron wrapper around vite-demo
├── package.json       (pnpm workspace root)
└── pnpm-workspace.yaml
```

`ts/` is a pnpm workspace; packages reference each other via `workspace:*`.

## Packages

### `@anyfs/core`

The runtime. Exports a wasm kernel build of anyfs (LKL + QEMU block layer + 35
filesystems) plus a thin JS surface for mounting disks and walking the VFS.

Ships four wasm bundles via package `exports`:

| Export                              | Target  | QEMU block layer | Notes                                  |
| ----------------------------------- | ------- | :--------------: | -------------------------------------- |
| `@anyfs/core/wasm/anyfs.mjs`        | browser | yes              | Runs inside a dedicated Web Worker     |
| `@anyfs/core/wasm/anyfs.worker.js`  | browser | yes              | Worker bootstrap for the above         |
| `@anyfs/core/wasm/anyfs.node.mjs`   | Node    | yes              | Same wasm, Emscripten Node environment |

The browser bundle **must** run in a dedicated Web Worker — `WORKERFS` asserts
`ENVIRONMENT_IS_WORKER`, and Chrome forbids `Atomics.wait` on the main thread.
A `worker-client.ts` helper handles message passing.

Source layout (`packages/core/src/`):

- `boot.ts`         — kernel bringup + dmesg routing
- `disk.ts`         — `anyfs_disk_*` bindings (add/list/meta/remove)
- `module.ts`       — Emscripten module wrapper
- `url-fs.ts`       — sync XHR + 512 KiB Range + 32-chunk LRU; mounts http(s) disk images
- `worker.ts`       — runs inside the Web Worker
- `worker-client.ts`— main-thread proxy
- `node.ts`         — Node entrypoint
- `index.ts`        — public re-exports

Build:

```bash
pnpm -C ts -F @anyfs/core build   # tsup TS bundle; wasm is committed pre-built
pnpm -C ts -F @anyfs/core test    # smoke-tests against single/multi/big disk images
```

The wasm itself is produced out-of-band by
`scripts/build_anyfs_browser_wasm.sh` (browser, qemu variant) and the
companion node variant — see `scripts/build_lkl_wasm.sh` for the kernel side.

### `@anyfs/react`

React 18/19 hooks over `@anyfs/core`. Source (`packages/react/src/`):

- `provider.tsx`  — `<AnyfsProvider>` boots the worker, exposes disk handles
- `use-dir.ts`    — directory listing hook (lkl `getdents` → SWR-ish cache)
- `use-file.ts`   — file read hook (chunked, suspense-friendly)

Build:

```bash
pnpm -C ts -F @anyfs/react build
```

### `@anyfs/trees`

A drop-in `<AnyfsFileBrowser>` component built on
[Chonky 2.3.2](https://github.com/TimboKZ/Chonky) +
`chonky-icon-fontawesome`. Hands a mounted disk root to Chonky and wires
double-click into `useDir` / `useFile`.

Known footguns (from working on this UI):

- Chonky 2.3.2 silently drops `FileData` entries with falsy `name`. Use ZWSP
  (`'​'`) for icon-only crumbs.
- Chonky 2.3.2's auto-extname splitter prepends `.` to extensionless names —
  set `ext` on `FileData` yourself to short-circuit it.

### `@anyfs/native` (private, not published)

A Node N-API addon that links against the native `liblkl.so` /
`libanyfs-qemublk.so` instead of the wasm build. Same JS surface as
`@anyfs/core`, ~10× the throughput, Linux/macOS only. Built with `node-gyp`.

```bash
pnpm -C ts -F @anyfs/native build   # node-gyp rebuild
pnpm -C ts -F @anyfs/native test
```

## Examples

### `examples/vite-demo`

Vite + React 19 + Tailwind 3. The deployed app at
<https://anyfs.kosaka.moe> serves this build. It boots `@anyfs/core` during the
landing screen (`project_anyfs_landing_prewarm`), accepts file drops and HTTP
URLs (`@anyfs/core/url-fs`), and renders the disk tree with `@anyfs/trees`.

```bash
pnpm -C ts -F vite-demo dev       # http://localhost:5173
pnpm -C ts -F vite-demo build     # static output in ts/examples/vite-demo/dist
```

The wasm bundle is loaded via `anyfs.qemu.mjs` (the QEMU variant). If you
modify any of the C glue, rebuild with `ANYFS_QEMU=1` or the demo will keep
running the stale wasm.

### `examples/electron-demo`

Electron wrapper around the vite-demo build, packaged for Linux x64 and macOS
arm64 via `@electron/packager`. The renderer is loaded through a custom
`anyfs://` protocol — `file://` can't carry the COOP/COEP headers Atomics
needs.

```bash
pnpm -C ts -F electron-demo dev          # vite + electron, live reload
pnpm -C ts -F electron-demo package      # Linux x64
pnpm -C ts -F electron-demo package:mac  # macOS arm64
```

`preload.ts` is currently a placeholder; the renderer talks to `@anyfs/core`
the same way the browser demo does. A future iteration will bridge to
`@anyfs/native` via N-API.

## Workspace commands

```bash
cd ts
pnpm install                  # one-time
pnpm -r --filter './packages/*' build    # build all libraries
pnpm -r --filter './packages/*' test     # smoke tests
```
