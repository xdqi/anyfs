# @anyfs/e2e — End-to-end test suite

Playwright E2E suite for the two shipping front-ends:

- **web** — the browser app (`ts/examples/vite-demo`), running `@anyfs/core` wasm in a Web Worker.
- **electron** — the desktop app (`ts/examples/electron-demo`), which can use the native LKL
  N-API addon **or** the same wasm fallback.

Each user-flow spec is written **once** against a `Driver` interface and runs across three
Playwright projects, so the same journey is exercised on every backend:

| Project | Backend | How it launches |
| --- | --- | --- |
| `web` | wasm Web Worker | `vite build` + `vite preview` on port 4199 (prod bundle; `?e2e=1` opts in the debug hook) |
| `electron-native` | native LKL addon (`anyfs_native.node`) | Playwright `_electron` on the esbuilt `dist/main.cjs` |
| `electron-wasm` | wasm fallback (`ANYFS_DISABLE_NATIVE=1`) | same, native disabled |

## Layout

```
ts/tests/e2e/
├── playwright.config.ts     # 3 projects; web webServer (build+preview, reuseExistingServer:false)
├── drivers/
│   ├── driver.ts            # Driver interface + DownloadResult/ErrorKind/PropsInfo/RowInfo
│   ├── web-driver.ts        # drives vite-demo via __anyfsTest + DOM, SW download capture
│   ├── electron-driver.ts   # drives electron-demo via _electron, IPC download capture
│   └── dom-actions.ts       # Chonky/DOM actions shared by both drivers (DRY)
├── lib/
│   ├── test-fixture.ts      # `driver` fixture: picks WebDriver/ElectronDriver per project
│   ├── launch-electron.ts   # launchElectron(backend, localImagePath?)
│   ├── native-guard.ts      # electron-native FAILS loudly if no ABI-matching addon
│   ├── electron-image.ts     # setElectronImage() seam (image path known at launch time)
│   ├── assertions.ts        # expectKnownTree(driver, part)
│   └── paths.ts             # repo path helpers
├── fixtures/
│   ├── manifest.ts          # the single source of truth: each fixture + expected tree
│   ├── generate.mjs         # root mkfs/loop builder (generated images)
│   ├── fetch.mjs            # downloads + caches the remote images (sha/size guarded)
│   ├── ensure.ts            # ensureFixture(name): build-or-fetch + verify guards
│   ├── range-server.ts      # serveFileWithRange / serveFileNoRange (URLFS tests)
│   ├── bad-image.ts         # makeBadImage(): 1 MiB of zeros (error flow)
│   └── images/              # built/downloaded images (gitignored)
├── flows/                   # written once, run across all 3 projects
│   ├── open-browse-download.spec.ts
│   ├── url-load.spec.ts
│   ├── formats.spec.ts
│   └── errors.spec.ts
├── electron-only/
│   └── backend-switch.spec.ts   # native→wasm switch (electron-native only)
└── FINDINGS.md              # real app behaviour the suite surfaced (bugs found + fixed/open)
```

## Prerequisites

- **Node + pnpm** (pnpm 11.2.2). Run `pnpm install` at `ts/`.
- **Playwright Chromium** — `cd ts/tests/e2e && npx playwright install chromium` if not already present.
- **A display for Electron** — on a headless box, run Electron projects under `xvfb-run -a`.
- **Fixture generation needs passwordless `sudo`** plus the mkfs tooling
  (`gdisk dosfstools e2fsprogs btrfs-progs qemu-utils` — `mkfs.ext4`/`mkfs.fat`/`mkfs.btrfs`/
  `sgdisk`/`sfdisk`/`qemu-img`; on Debian these live in `/sbin`, which the generator invokes via
  `sudo`). The downloaded fixtures need network (~850 MB total; cached + sha-guarded).
- **Native addon for `electron-native`** — `cd ts/packages/anyfs-native && npx node-gyp build`
  produces `build/Release/anyfs_native.node`. The native guard **fails loudly** if it's missing
  rather than silently skipping (ABI drift would otherwise SIGSEGV at `require()`).

## Fixtures

Build/download everything up front:

```sh
cd ts/tests/e2e
pnpm fixtures            # = node fixtures/generate.mjs && node fixtures/fetch.mjs
```

(Specs also lazily call `ensureFixture(name)`, which builds-or-fetches on demand and verifies the
size/sha guard before use.)

| Fixture | Source | Container | Exercises |
| --- | --- | --- | --- |
| `multiRaw` (`multi.img`) | generated | GPT | ext4 (p1) + vfat (p2); GPT parse, multi-partition |
| `mbrExtended` (`mbr-extended.img`) | generated | MBR | 2 primary + 2 logical in an extended partition (EBR chain walk) |
| `btrfsVmdk` (`btrfs-whole.vmdk`) | generated | none (whole-disk), VMDK | whole-disk btrfs, vmdk decode |
| `qcow2Url` (`trusty-cloud.qcow2`) | downloaded | qcow2 | qcow2 decode + URLFS Range |
| `isoUrl` (`trusty.iso`) | downloaded | ISO9660 | iso path + URLFS Range |

## Running

```sh
cd ts/tests/e2e

# everything (web auto-builds + previews the prod bundle; run the whole thing under xvfb so the
# electron projects have a display)
xvfb-run -a npx playwright test

# one project
npx playwright test --project=web
xvfb-run -a npx playwright test --project=electron-wasm
xvfb-run -a npx playwright test --project=electron-native

# fast smoke subset, no live internet
xvfb-run -a npx playwright test --grep @smoke --grep-invert @network
```

package.json scripts: `test`, `test:web`, `test:electron`, `test:smoke`, `fixtures`.

- The web project always spawns its own fresh `vite build && vite preview` on **port 4199**
  (`reuseExistingServer: false`) so a run never tests stale content.
- `@network` tags a single url-load variant that hits the real remote URL; the default suite is
  hermetic — exclude it with `--grep-invert @network`.

## Backend matrix (what currently passes vs. `test.fixme`)

| Flow | web | electron-wasm | electron-native |
| --- | --- | --- | --- |
| open-browse-download | smoke + properties pass; **download skipped (F4)** | **all pass** (incl. 13-byte IPC download) | fixme (F9) |
| url-load | fixme (F10) | fixme (F10) | fixme (F9/F10) |
| formats (multiRaw GPT, mbrExtended MBR) | **pass** | **pass** | fixme (F9) |
| formats (btrfsVmdk) | fixme (F10) | fixme (F10) | fixme (F5/F9) |
| errors (corrupt image, no-range URL) | **pass** | **pass** | fixme (F9) |
| backend-switch | n/a (electron-only) | skipped (starts from native) | **pass** (relaunch request + post-relaunch wasm browse) |

The URLFS *mechanism* works (source attaches, mounts, reaches ready); url-load is fixme only
because its qcow2 fixture isn't decoded on the wasm WORKERFS/URLFS path (F10). MBR extended +
logical partitions (`mbrExtended` #5/#6) genuinely enumerate, mount, and list.

## Findings

The suite surfaced real app behaviour — see **`FINDINGS.md`** for full detail. Summary:

**Found and FIXED during the suite build:**

- **F1 / F2 / F3** — wasm ext4 / no-FS / btrfs mount **hung** (kthread-spawn-while-blocked
  deadlock in the worker). Fixed (`03c1591`) by running `session_enter` on a dedicated pthread.
- **F6** — IndexedDB never settles on the `anyfs://` origin, hanging the Electron "Open file…"
  button. Fixed (`06e792e`) by time-boxing `openDb()`.
- **F7** — native partition mount **crashed** the Electron main process (QEMU's libblock
  AioContext dispatched by Electron's GLib loop collided with QEMU's io_uring fdmon →
  `get_sqe` abort). Fixed (`06e792e`) by reverting `sessionEnter` to a synchronous N-API call.
- **F8** — electron-wasm rejected local file sources (path-only). Fixed (`de735a4`) by feeding a
  blob source when native is disabled.

**Open (documented, lower severity):**

- **F4** (Medium) — web Service-Worker download fails under `vite preview` (missing
  `Service-Worker-Allowed: /`). Download is verified instead via the Electron IPC path.
- **F5** (Medium) — native whole-disk btrfs (no partition table) isn't auto-detected (missing
  btrfs magic in the open-time hint probe); errors cleanly, doesn't hang.
- **F9** (Medium) — Electron `app.close()` hangs ~2 min after a *native* mount (the teardown tail
  of F7's QEMU↔GLib entanglement). The native app mounts/browses/downloads fine in isolation;
  only clean teardown hangs, so the electron-native flows are `test.fixme`. Proper fix = run the
  addon in a separate process (utilityProcess) with its own libuv loop.
- **F10** (root cause open) — qcow2/vmdk isn't decoded on the wasm WORKERFS/URLFS open path (the
  image is read as raw bytes, so its partition table never enumerates). QEMU *is* in the wasm
  bundle; the gap is backend selection on that path.

## CI

CI wiring is intentionally **out of scope** for now. The `@smoke` tags and `test:smoke` script
exist for a future gate — a reasonable one is
`xvfb-run -a npx playwright test --grep @smoke --grep-invert @network` on `web` + `electron-wasm`
(the wasm bundle is prebuilt/committed, so no LKL compile is needed; fixture generation needs
`sudo` + mkfs tooling on the runner).
