# tests/diagnostics — manual diagnostic scripts

**The Playwright E2E suite at `ts/tests/e2e` is the primary regression suite.**
The scripts in this directory are kept as *manual diagnostics* — they are not part
of any CI or `pnpm test` gate, but they remain runnable and are useful when the
Playwright harness itself is in question (raw CDP needs no Playwright install) or
when debugging a backend the Playwright suite currently gates off (see the
electron-native / F9 notes below).

## CDP UI suite (demoted 2026-06-10)

The legacy UI automation suite drives the real apps over raw CDP (no
dependencies beyond Node + a Chromium/Electron binary):

- `test-cdp.mjs` — one target×source combo per invocation
- `run-all.mjs` — driver for all 6 combos
- `common-cdp.mjs` — minimal dependency-free CDP client (WebSocket framing,
  `Runtime.evaluate`, console capture). Also imported by the boot/prewarm
  debug scripts below (`common.mjs` re-exports it alongside the
  Electron/vite/HTTP-server launchers).

### How to run

```sh
# everything (6 combos; needs Xvfb for the electron targets)
node tests/diagnostics/run-all.mjs [--image=path/to/disk.img]

# one combo
node tests/diagnostics/test-cdp.mjs --target web      --source local [--image ...]
node tests/diagnostics/test-cdp.mjs --target web      --source http  [--image ...]
node tests/diagnostics/test-cdp.mjs --target electron --mode wasm   --source local
node tests/diagnostics/test-cdp.mjs --target electron --mode native --source http [--url https://...]
```

Default image: `tests/images/ext4.img`. The scripts spawn their own vite dev
server / Electron / headless Chromium and `pkill` stale ones first — do **not**
run them next to a live dev session you care about.

## Parity audit (why the suite was demoted)

Every assertion the CDP suite makes was mapped to the Playwright suite
(`ts/tests/e2e`, projects `web` / `electron-wasm` / `electron-native` over
`flows/*.spec.ts` + `electron-only/backend-switch.spec.ts`). Result: full
parity — every behavioural assertion is covered (usually strictly stronger),
and the one genuine gap found (the "Open URL…" dialog *UI* flow) was ported
into `flows/url-load.spec.ts` as part of this audit.

| # | CDP assertion | Combos | Playwright coverage |
| --- | --- | --- | --- |
| C1 | Page title contains "anyfs" | all 6 | **Not ported (justified):** branding text, not behaviour. Every driver's `start()` waits for the `__anyfsTest` bridge — a strictly stronger "app actually booted" signal than a title substring. |
| C2 | Kernel boots (`Linux version` dmesg in console, or `crossOriginIsolated` fallback) | all 6 | Covered implicitly and strictly stronger: every flow mounts a partition and lists pinned filenames (`expectKnownTree`), impossible without a booted kernel. `open-browse-download.spec.ts` `@smoke` runs on all 3 projects. |
| C3a | Local open via "Open file…" button (native IPC dialog seam `ANYFS_TEST_LOCAL_PATH`) | electron-native-local | Covered by `electron-driver.ts` `openImage()` via the `__anyfsTest.openPath` bridge — sets the *same* `{kind:'path'}` source the button's IPC produces; the button itself is deliberately bypassed (FINDING F6, recents/IndexedDB hang — documented in the driver). Native runs are `test.fixme` (F9 teardown hang); this CDP combo stays the manual way to exercise native until the utilityProcess split lands. |
| C3b | Local open on wasm targets (served over local HTTP + `openUrl` hook — a CDP workaround for File-postMessage cloning) | web-wasm-local, electron-wasm-local | Covered *better*: Playwright drives the real local-file path (`setInputFiles` on the hidden `legacy-file-input`) in `open-browse-download.spec.ts` / `formats.spec.ts`; the URLFS-over-local-HTTP aspect is covered by `url-load.spec.ts` (local Range server). |
| C3c | HTTP open via `__anyfsTest.openUrl` hook, with UI fallback ("Open URL…" button → `aria-label="Disk image URL"` input → dialog Open) | all `*-http` | Hook path: `url-load.spec.ts` `@smoke` (web + electron-wasm; native F9-gated). UI dialog path: **GAP — closed by this audit**: `url-load.spec.ts` "open via the \"Open URL…\" dialog UI" (web; the dialog is identical renderer DOM on every shell). |
| C3d | electron-wasm external URL wrapped as `anyfs-url://proxy/?u=…` | electron-wasm-http | **Not ported (justified):** the wrapper was a CDP-test-level CORS workaround for *external* URLs. Playwright's hermetic Range server sends CORS headers, so raw URLs work (`url-load.spec.ts` passes on electron-wasm); the real-remote case is the `@network`-tagged variant. |
| C4 | First partition button clickable (`#N` button, optional/non-fatal) | `*-local` | Covered strictly stronger: `listPartitionIndices()` asserts the expected indices and `formats.spec.ts` enters **every** manifest-pinned partition (GPT multi, MBR extended/logical) plus `backToPartitions()` round-trips. |
| C5 | File tree appears (>0 rows, or fuzzy filename text in body) | all 6 | Covered strictly stronger: `expectKnownTree` asserts manifest-pinned filenames per partition; downloads then prove content (13-byte `hello.txt`). |
| C6 | No JS errors (`window.__jsErrors` empty) | all 6 | **Not ported (justified):** non-fatal even in the CDP suite ("may be headless artifacts" — it only logged on failure, never failed the run). Playwright traces capture page errors for debugging; a hard zero-JS-error assertion would be flaky by the CDP suite's own admission. |

Combo-level note: the Playwright suite runs the shared flows on `web` and
`electron-wasm` green; `electron-native` is `test.fixme` solely on FINDING F9
(Electron `app.close()` hangs ~2 min after a native QEMU+LKL mount — a real
shutdown defect, not missing coverage; the assertions exist in the specs and
the fixme is removed once the addon moves to a utilityProcess). The CDP suite
never observed F9 because it kills the Electron process instead of closing it
— which is exactly why it stays here as a native-backend diagnostic.

## Debug scripts

Ad-hoc boot/prewarm repro scripts (moved here from `tests/` 2026-06-10). Each
spawns its own vite dev server and/or Electron under Xvfb and `pkill`s stale
ones first. One line per script — the regression it guards:

- `test-atomics.mjs` — Atomics.wait-on-main-thread regressions: drives
  `debug-atomics.html` in Electron and watches its log (wasm LKL must never
  block the main thread; see the dedicated-Worker rule).
- `test-async-boot.mjs` — the workeronly async boot path: Electron →
  `anyfs.worker.js` → `anyfs.workeronly.mjs` must complete async kernel boot.
- `test-prewarm-direct.mjs` — landing prewarm, direct harness: Electron on
  `debug-worker.html`, watches the prewarm worker boot to completion.
- `test-prewarm-e2e.mjs` — landing prewarm, end-to-end: vite-demo main page
  must pre-boot the wasm LKL kernel during the landing screen.
- `test-direct-module.mjs` — main-page module load failure mode: loads
  `anyfs.mjs` directly on the main page (not in a Worker) and calls
  `anyfs_ts_init`. **Expected to fail by design** — the wasm bundle must live
  in a dedicated Web Worker (WORKERFS asserts, Atomics.wait forbidden on the
  main thread); this script documents that failure signature.
- `test-worker-debug.mjs` — worker boot logging: minimal Electron run against
  `debug-worker.html` capturing verbose worker console output while waiting
  for the prewarm result.
