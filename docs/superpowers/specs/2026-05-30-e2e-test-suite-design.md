# E2E Test Suite Design — Web (vite-demo) + Electron (electron-demo)

**Date:** 2026-05-30
**Status:** Approved design, ready for implementation plan

## Goal

Prove that both shipping front-ends — the browser app (`ts/examples/vite-demo`) and the
Electron desktop app (`ts/examples/electron-demo`) — work as a user sees them: open a disk
image, browse the filesystem tree, inspect file properties, and download a file. Cover the
real backend paths (wasm Web Worker, Electron native N-API addon, Electron wasm fallback) and
the network (URLFS) path. Wire a cheap subset into CI as a regression gate.

Today neither target has a real test harness — only `__anyfsTest` CDP hooks, env-var smoke
paths in the Electron main process, and standalone Node scripts for the native addon.

## Scope

**In scope:** E2E user flows across web + Electron, plus a CI smoke gate.

**Out of scope:** React component unit tests with the session layer mocked; protocol-only
integration tests (the worker RPC / IPC surfaces are exercised transitively through the E2E
flows, not unit-tested in isolation here).

## Harness

Playwright for both targets, one framework / one assertion API:

- **Web** — Chromium headless against a `vite preview` server that emits the COOP/COEP headers
  required for `SharedArrayBuffer` + `Atomics.wait` (cross-origin isolation). A startup guard
  fails fast if isolation is missing, because the wasm pthread path dies silently otherwise.
- **Electron** — Playwright's `_electron.launch()` runs the built main process and lets tests
  evaluate in both main and renderer contexts.

## Architecture (Approach A: one config, three projects, shared driver)

A single new test package at `ts/tests/e2e/`, sibling to the existing
`ts/tests/native-session.test.mjs`. Because `ts/tests/` is **not** currently a pnpm workspace
member, this work adds `tests/*` to `ts/pnpm-workspace.yaml` and extends the root prettier globs
to cover it.

```
ts/tests/e2e/
├── playwright.config.ts        # 3 projects: web, electron-native, electron-wasm
├── package.json                # @playwright/test; scripts: test, test:web, test:electron, fixtures
├── fixtures/
│   ├── generate.mjs            # root/sudo mkfs + loop builder → emits images + verifies manifest
│   ├── fetch.mjs               # downloads + caches remote images, sha/size guard
│   ├── manifest.ts             # registry: each fixture's path/url + expected tree & file sizes
│   ├── range-server.ts         # local HTTP server honoring Range, serves cached images for URLFS
│   └── images/                 # generated + downloaded images (gitignored, built/fetched on demand)
├── drivers/
│   ├── driver.ts               # Driver interface
│   ├── web-driver.ts           # drives vite-demo via __anyfsTest + DOM
│   └── electron-driver.ts      # drives electron-demo via _electron, parametrized by backend env
├── flows/                      # written ONCE against Driver, run across all projects
│   ├── open-browse-download.spec.ts
│   ├── url-load.spec.ts
│   ├── formats.spec.ts
│   └── errors.spec.ts
├── electron-only/
│   └── backend-switch.spec.ts  # native↔wasm via Settings disableNative → relaunch
└── lib/
    ├── assertions.ts           # expectKnownTree, expectFileDownloaded(name,size), ...
    └── headers.ts              # COOP/COEP guard for the web preview server
```

### The `Driver` boundary

Every flow spec talks only to a `Driver`. The two implementations differ in *how* but expose
the same verbs. Crucially the abstraction does **not** hide real mechanism differences — e.g.
`download()` reports which mechanism actually fired so the flow can assert the correct one per
project (Service Worker on web, IPC on Electron).

```ts
type DownloadMechanism = 'service-worker' | 'electron-ipc';

interface DownloadResult {
  bytes: Uint8Array;        // captured file content
  size: number;
  mechanism: DownloadMechanism;
}

interface Driver {
  openImage(fixture: Fixture): Promise<void>;       // file-picker / drag-drop / native dialog
  openUrl(fixture: UrlFixture): Promise<void>;      // URLFS path (local range server or @network remote)
  listParts(): Promise<PartInfo[]>;
  enterPart(idx: number): Promise<void>;
  readTree(): Promise<TreeNode[]>;                  // entries in the current directory
  navigate(relPath: string): Promise<void>;
  statFile(relPath: string): Promise<Stat>;         // Properties dialog
  download(relPath: string): Promise<DownloadResult>;
  expectError(kind: ErrorKind): Promise<void>;      // bad image / no-range / unsupported
}
```

- `WebDriver` injects the source via `window.__anyfsTest` (`setSourceFile` / `openUrl` / `openPath`),
  then drives partition selection, tree navigation, Properties, and download through the DOM.
  `download()` asserts the Service-Worker mechanism and captures bytes via
  `page.waitForEvent('download')`.
- `ElectronDriver` feeds the image without a native OS dialog, drives the same renderer DOM, and
  `download()` asserts the Electron IPC path (`electronDownload.open/write/close`) by writing to a
  temp dir the test reads back. It has **two source-injection seams with different fidelity**
  (see below) and uses the more faithful one by default.

### Debug-hook policy (`__anyfsTest`)

The tests stay **true E2E**: all *actions* — selecting a partition, navigating the tree, opening
Properties, downloading — go through the real DOM (Chonky rows, dialog buttons). The
`__anyfsTest` surface is kept **minimal, additive, and read-mostly**, used only for seams that
cannot be driven faithfully otherwise.

Changes to the app under test (`ts/examples/vite-demo/src/App.tsx`):

1. **Keep the existing 3 source setters** — `openUrl` / `openPath` / `setSourceFile`. Scripting a
   native OS file dialog or a real drag-drop is not reliable; injecting the source is the
   standard, legitimate seam, and it is the *entry* of every flow.
2. **Add read-only observability** (no action-replacing verbs):
   - `__anyfsTest.getState()` → `{ status, mode, mountPath, error }` mirroring the existing
     `AnyfsState` (provider.tsx): `status` is `AnyfsDiskStatus`
     (`idle`/`booting`/`booted`/`attaching`/`mounting`/`ready`/`error`), `mode` is
     `AnyfsBackendMode` = `'native'|'wasm'|'node-wasm'`, `mountPath: string|null`,
     `error: Error|null`. The provider already computes all of these; the hook surfaces existing
     values, it does not invent new state. Because `AnyfsState` lives in React state (not an
     imperatively-readable object), the hook reads it through a `useRef` that the provider keeps
     in sync — the cleanest place is a tiny bridge component rendered inside `<AnyfsProvider>`
     that calls `useAnyfsDisk()` and writes the snapshot to the ref on every change. Lets the
     driver *await* `status === 'ready'` deterministically instead of polling with sleeps, and
     lets the Electron projects **assert which backend actually loaded** (`mode`) — essential for
     native-vs-wasm coverage and the switch flow.
   - `__anyfsTest.lastError` → reads `state.error` (already in `AnyfsState`), so the error-flow
     specs assert failure deterministically rather than scraping dialog text.
3. **Gate the hook behind a flag** — today `__anyfsTest` is attached unconditionally on every
   page load. Change it to attach only when `import.meta.env.DEV || (URL has ?e2e=1)` (see
   "Gating mechanism" below), so the debug surface is not live in normal production sessions.

Explicitly **rejected**: rich action hooks (`listParts`/`enterPart`/`readdir`/`stat`/
`openReadable`) that bypass the UI — they would make the suite fast and stable but no longer
E2E (a broken Chonky row click or download button would pass). Actions stay on the DOM.

### Electron source-injection seams: `ANYFS_TEST_LOCAL_PATH` vs `__anyfsTest.openPath`

The Electron native-open path is `electronDialog.openImage()` → `dialog:openImage` IPC →
**native OS file dialog** ([main.ts:301-338](../../../ts/examples/electron-demo/src/main.ts)). A
native dialog is an OS window with no DOM — Playwright/CDP cannot drive it. The two seams differ:

| Seam | Drives | Fidelity |
|---|---|---|
| `ANYFS_TEST_LOCAL_PATH=<p>` (env, main proc) | the **real `dialog:openImage` IPC handler** returns `<p>` instead of popping the dialog | high — exercises the full dialog→IPC→`attachPath` native pipeline |
| `__anyfsTest.openPath(<p>)` (renderer) | sets the React `source` directly | lower — skips the dialog→IPC handshake |

`ANYFS_TEST_LOCAL_PATH` is therefore **necessary** (not redundant with `openPath`): it is the
only way to cover the real native-open IPC pipeline headlessly. Driver policy:

- **open-browse-download / formats** on Electron → use `ANYFS_TEST_LOCAL_PATH` (faithful path).
- **switch flow** post-relaunch wasm launch, and any case that deliberately bypasses the dialog →
  use `__anyfsTest.openPath`.

### Gating mechanism (web vs Electron — different runtimes, each idiomatic)

The browser renderer has no `process.env`; the Electron main process has no `import.meta.env`.
So a single mechanism cannot cleanly span both. Each layer uses its idiomatic gate:

- **Web renderer** (`__anyfsTest`, `loglevel` — see cleanups): attach/verbose **iff**
  `import.meta.env.DEV` (Vite build-time dev flag, true under `vite dev`) **OR** the page URL
  carries `?e2e=1` (our own runtime opt-in param — *not* a Playwright built-in; the app reads
  `URLSearchParams` itself). The `?e2e=1` path is what lets Playwright opt the hook on against the
  **production `vite preview` build** (where `import.meta.env.DEV` is `false`), preserving
  prod-bundle fidelity. The driver navigates to `/?e2e=1`. **`window.__ANYFS_E2E` is dropped** —
  `?e2e=1` is the single web opt-in.
- **Electron main** (`ANYFS_TEST_LOCAL_PATH`, the SMOKE vars): keep `process.env` — the
  established convention in this file; the main process has no `import.meta.env`.

### Debug-infrastructure inventory + cleanups

Full audit of debug/test-only infrastructure currently in both apps (verified 2026-05-30):

**Electron** — `ANYFS_TEST_LOCAL_PATH` (main.ts:305), `ANYFS_DRIVES_SMOKE`/`ANYFS_DRIVES_OUT`
(556-559), `ANYFS_NATIVE_SMOKE`/`ANYFS_NATIVE_IMAGE`/`ANYFS_NATIVE_OUT` (574-588),
`ANYFS_DISABLE_NATIVE` (preload.ts:78, also a real user setting), `ELECTRON_DEV` (22). These stay
as-is — they are env-gated and not shipped-on by default.

**Web** — three items currently leak into the production build and are cleaned up as part of this
work (user-approved):

1. **`__anyfsTest` attached unconditionally** (App.tsx:46) → gate per the mechanism above.
2. **`mountOpts={{ loglevel: 7 }}` hardcoded** (App.tsx:142) → max kernel log verbosity shipped to
   prod. Gate the same way: verbose (7) under DEV/`?e2e=1`, a quieter default in normal prod.
3. **8 orphaned debug/probe HTML pages shipped in `dist/`** — `debug.html`, `debug-api.html`,
   `debug-atomics.html`, `debug-worker.html`, `debug-stream.html`, `direct-module-test.html`,
   `probe.html`, `probe2.html` (in `public/`, not linked from the app, but bundled into prod) →
   exclude from the production build (move out of `public/`, or a Vite build exclude). They remain
   available for manual debugging in dev.

## Prerequisites discovered during planning (verified 2026-05-30)

Source inspection surfaced facts that become explicit prerequisite tasks in the plan:

- **No `data-testid` exists anywhere** in vite-demo or `@anyfs/trees`. The plan adds stable
  testids as a prerequisite: file-picker open buttons (`open-file-button`/`open-url-button`/
  `open-drives-button`), partition rows (`partition-<index>`), URL dialog submit, download status
  container + cancel, settings disable-native toggle + restart-confirm, the URL error dialog, and
  the properties modal. The Chonky file list has no per-row testids and uses an internal shadow
  DOM; rows are addressed by their Chonky-assigned `id` (mount-relative path) / text — the plan
  uses Chonky's row identity, not invented testids, for file rows.
- **Electron download is blocked by `dialog.showSaveDialog()`** ([main.ts `download:open`]) with
  **no existing bypass** (unlike `ANYFS_TEST_LOCAL_PATH` for open). The plan **adds
  `ANYFS_TEST_DOWNLOAD_DIR`**: when set, `download:open` writes to `<dir>/<fileName>` and skips
  the dialog. The test reads the file back to assert bytes/size.
- **wasm bundle is prebuilt and committed** (`packages/core/wasm/*`, ~58 MB `anyfs.wasm`,
  symlinked into `vite-demo/public/wasm/`). Web E2E runs in CI **without compiling LKL**.
- **No Playwright / node-CI infra exists**; `.github/workflows/linux.yml` is pure C build. The
  plan adds a separate Node/Playwright CI job. Electron under Playwright needs a display in CI →
  `xvfb-run`.
- **State shape** is `AnyfsState` with `status` (not `phase`) and `mode` ∈
  `'native'|'wasm'|'node-wasm'`; consumed via `useAnyfsDisk()`. The `getState()` hook reads it
  through a ref bridge (see Debug-hook policy).

## Launch modes

- **Web** — `vite build` then `vite preview` (tests the real production bundle; the driver opts
  the debug hook in via `/?e2e=1`). A `headers.ts` guard asserts cross-origin isolation
  (COOP/COEP) is present on the preview server before running.
- **Electron** — `_electron.launch()` on the esbuilt `dist/main.cjs` (via `pnpm build:main` +
  `build:renderer`), with `ELECTRON_DEV`-style env. **No `electron-packager` step** — the plan
  drives the built main process directly, not the packaged binary.

## Backend wiring

Three Playwright projects:

| Project | Launch | Backend selector |
|---|---|---|
| `web` | `vite preview` + Chromium (`/?e2e=1`) | wasm Web Worker (only path) |
| `electron-native` | `_electron.launch(dist/main.cjs)` | native addon (default) |
| `electron-wasm` | `_electron.launch(dist/main.cjs)` | `ANYFS_DISABLE_NATIVE=1` (wasm fallback) |

The four flow specs run across all three projects. `backend-switch.spec.ts` runs only in the
Electron projects.

**Native precondition guard.** The `electron-native` project verifies that an
ABI-matching `anyfs_native.node` (built against the test's Electron version) is present. If not,
it **fails with a clear message** rather than silently skipping — addon/Electron ABI drift
otherwise SIGSEGVs during `require()`.

## Backend-switch flow

`_electron` cannot follow a real process re-exec, so the relaunch is tested in two halves:

1. Launch native → open Settings → toggle `disableNative` → confirm dialog → assert the
   `settings:relaunch` IPC fired with the expected args (i.e. `app.relaunch()` + `app.exit()`
   were requested).
2. Separately launch a fresh Electron instance with `ANYFS_DISABLE_NATIVE=1` and assert it boots
   on the **wasm** backend and can open an image and browse it.

Together this proves both the relaunch *request* and the resulting post-relaunch *state*.

## Fixtures

Image generation may use **root/sudo** — real `mkfs` + loop devices (`losetup`, `mount`,
`sfdisk`/`fdisk`/`sgdisk`, `mkfs.ext4`/`mkfs.fat`/`mkfs.btrfs`). The spec documents this root
requirement explicitly; unprivileged CI must provide privileges or skip generation.

Each fixture has a manifest entry pinning its expected directory tree and exact file sizes
(and content hashes where useful) so assertions are exact. The full matrix:

| Fixture | Source | Container / PT | Filesystem(s) | Exercises |
|---|---|---|---|---|
| `multiRaw` | generated | GPT | ext4 + vfat | GPT parse, multi-partition, ext4/fat drivers |
| `mbrExtended` | generated | MBR (msdos) | 2 primary + N logical | MBR parse, extended-partition / EBR chain walk, logical-partition enter |
| `btrfsVmdk` | generated | **none** (whole-disk), VMDK wrap | btrfs | vmdk decoder, no-partition-table / whole-disk path, btrfs driver |
| `qcow2Url` | downloaded | (ubuntu cloud image) | (image's own) | qcow2 decoder + URLFS Range |
| `isoUrl` | downloaded | ISO9660 | iso9660 | iso path + URLFS Range |

**Generated images** — built on demand into gitignored `fixtures/images/`. Known file set per
filesystem includes a regular file with known content, a nested binary with a deterministic
pattern and known size, an empty directory (edge case), and a symlink (exercises
stat/realpath + the follow-symlinks toggle).

**Downloaded images** — fetched once and cached (gitignored) with a size/sha guard so a changed
or moved remote fails loudly rather than silently testing the wrong bytes. Pinned URLs:

- qcow2 (cloud image): `https://cloud-images.ubuntu.com/trusty/current/trusty-server-cloudimg-amd64-disk1.img`
- iso (release media): `https://releases.ubuntu.com/trusty/ubuntu-14.04.6-server-amd64.iso`

A `fixtures` npm script generates/fetches everything up front; specs use an
`ensureFixture(name)` helper that builds-or-fetches lazily and verifies the guard before use.

## User flows (spec files)

1. **open-browse-download** — open an image (file picker / drag-drop / native dialog), see the
   partition list, enter a partition, navigate the tree, open a file's Properties (stat), and
   download a file. Assert known files/sizes from the manifest, and that the *correct* download
   mechanism fired for the project.
2. **url-load (URLFS)** — open an image by URL. By default serve the cached fixture through the
   local Range-honoring server (hermetic, fast); a `@network`-tagged variant hits the two real
   remote URLs to prove real-world fetch. Assert the image mounts and browses correctly.
3. **formats** — open `multiRaw`, `mbrExtended`, `btrfsVmdk`, `qcow2Url`, `isoUrl`; assert each
   mounts and lists its known contents (covers GPT/MBR/no-PT topologies × ext4/vfat/btrfs/iso ×
   qcow2/vmdk decoders).
4. **errors / edge cases** — corrupt/bad image, URL without Range support (error dialog),
   unsupported format, empty partition/directory. Assert graceful failure, not just happy path.

## CI smoke gate

A fast, minimal subset wired into CI as a regression gate: boot each target and open a single
generated fixture (`multiRaw`) without crashing, asserting one known file is listed. Tagged
`@smoke` so it can run independently of the full (slower, network-touching) suite. The full
suite — including `@network` and the large downloaded images — runs on demand / nightly rather
than on every push.

## Honesty / correctness constraints

- `Driver.download()` reports the real mechanism (SW vs IPC); the flow asserts the right one per
  project. The driver never papers over this difference.
- The `electron-native` project fails loudly when no ABI-matching addon is present — never a
  silent skip that would let a broken native path masquerade as passing.
- Downloaded fixtures are guarded by size/sha; a changed remote fails the run rather than
  silently testing different bytes.
- The web project's COOP/COEP guard fails fast if cross-origin isolation is missing.

## Open question for the implementation plan

URL-load default: serve the cached copy via the local Range server (hermetic) with a `@network`
variant for the real remotes — as specified above. If a future decision wants the URL flow to
always hit the real remotes, drop the local-server indirection; not assumed here.
