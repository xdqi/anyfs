# wasm → web → electron CI design

**Date:** 2026-06-11
**Status:** approved section-by-section (architecture, prerequisites, all three jobs, guardrails)
**Workflow file:** `.github/workflows/wasm.yml` (new)

## Goal

Give the TS side of the repo a release pipeline: build the wasm bundle in CI,
package the vite-demo web app as a deployable tarball, and package the
electron demo for linux-x64 and win32-x64 (wasm mode only) — all in one
workflow run, artifacts only.

## Decisions (from design review)

| Question | Decision |
|---|---|
| What is the "web package"? | vite-demo `vite build` output (incl. wasm bundle) as a deployable tarball; feeds caddy directly |
| Where does the web job get the wasm bundle? | Same-run chaining: `web-package` needs `wasm-build`, artifact hand-off; every run rebuilds wasm |
| Electron scope | Both linux-x64 and win32-x64, **wasm mode only** (no `@anyfs/native`, no drivelist) |
| Triggers | `push: main` + `workflow_dispatch`; PRs stay on `ts.yml` unit tests |
| Publication | Workflow artifacts only (90-day retention); no releases, no npm, no auto-deploy |
| Workflow layout | Single new `wasm.yml` with a 4-job chain; `ts.yml` untouched |
| sccache for emcc | Yes for LKL + core compiles via `EM_COMPILER_WRAPPER` (validated locally); QEMU excluded this round (see research appendix) |
| sccache engine provisioning | `xdqi/sccache-dist-action` pinned **@v0.0.6** (engine assets built from the `sccache-dist-poc-tweaks` fork branch, includes the scheduler skew fix) |

## Architecture

```
wasm-build ──→ web-package ──→ electron-package (matrix: linux-x64, win32-x64)
 cold 40-70min / warm ~10-15min   ~5-10min          ~5min each, parallel
```

- All jobs on ubuntu-24.04. The win32 electron package is cross-packaged from
  Linux by electron-packager (the existing `package:win` script already runs
  this way).
- `concurrency: group: wasm-${{ github.ref }}, cancel-in-progress: true` so
  back-to-back pushes to main cancel the older run.

## Prerequisites (repo changes before the workflow can run)

1. **Commit `patches/qemu/` (9 emscripten patches) and `patches/linux/wasm/`**
   — both are currently untracked; `oot_fs.sh` already references the latter.
2. **New `scripts/build_qemu_wasm.sh`** — idempotently applies `patches/qemu/`
   to `deps/qemu` (marker mechanism like `oot_fs.sh`), runs the emscripten
   cross-file meson configure, and ninjas the static archives
   `build_anyfs_wasm.sh` consumes (`libblock.a`, `libqemuutil.a`,
   `libevent-loop-base.a`, …). The exact configure argv is recovered from the
   local `~/qemu/build-anyfs-wasm/meson-logs/` and frozen into the script.
   Paths via `lib/config.sh` (`QEMU_ROOT`/`EMSDK_DIR` overridable);
   shellcheck-gated.
3. **electron-demo `package.json`**: move `drivelist` and `@anyfs/native`
   from `dependencies` to **`optionalDependencies`**. `main.ts` lazy-loads
   both behind try/catch with wasm fallback, so runtime semantics are
   unchanged; this stops `pnpm install` on a bare runner from failing on the
   missing `file:../../../../drivelist-anyfs` path.
4. **electron-demo `package:wasm` / `package:win:wasm` scripts**: skip
   `stage:native*` / `copy-win64-dlls`, do not rebuild the renderer (consume
   the existing `../vite-demo/dist`; `stage:renderer` reused as-is).
5. **Small sync script: wasm bundle → `vite-demo/public/wasm/`** — replaces
   today's manual copy; shared by the web job and local dev.

## Job: `wasm-build`

Steps (all existing scripts, same order as the local flow):

1. checkout + `peru sync` (only the `linux` and `qemu` modules)
2. `scripts/oot_fs.sh` stage (OOT FS drivers + `patches/linux/wasm/series`)
3. `setup-emsdk@v14` pinned to **5.0.7** (same pin as `wasm-sysroot.yml`;
   bump deliberately, re-validate)
4. `scripts/fetch_wasm_ld.sh` + `scripts/fetch_wasm_sysroot.sh` (existing
   release-fetch pipelines)
5. `gen_lkl_config_wasm.sh` → `build_lkl_wasm.sh`
6. `build_qemu_wasm.sh` (new, prerequisite 2)
7. `build_anyfs_wasm.sh` (browser) + a second pass with `ANYFS_TARGET=node`
   (node variant feeds the smoke test)
8. **Gate:** wasm export-table check (`lib/wasm_exports.sh`, single source of
   truth, same gate as `linux.yml`). **Stretch:** node-target
   `smoke.node.mjs` if the fixture images can be generated in CI — failure to
   land this does not block the chain.
9. Upload artifact **`anyfs-wasm-bundle`** = the whole
   `ts/packages/core/wasm/` directory (~70 MB).

Caching:

- **sccache, local mode + SeaweedFS S3 backend** (existing secrets/infra).
  `EM_COMPILER_WRAPPER=sccache` covers the 1754 LKL kernel compile units and
  the core compiles in `build_anyfs_wasm.sh`. Engine binary provisioned from
  the `xdqi/sccache-dist-action` **v0.0.6** release assets. If the secret is
  missing or the backend is down, degrade to bare compiles — the cache must
  never break the build (same contract as `linux.yml`).
- **QEMU build dir via `actions/cache`** (key = peru qemu SHA +
  `patches/qemu` hash + script hash). QEMU is *not* routed through sccache
  this round — emcc injects `-Xclang -iwithsysroot…` into non-`-nostdinc`
  compiles and sccache's clang parser rejects those (see appendix). A
  whitelist patch to the sccache fork is a follow-up item.
- **`lkl-wasm/` build tree via `actions/cache`** (key = peru linux SHA +
  `patches/linux/wasm` hash + gen/build script hash). Complements sccache:
  the tree cache drives make's incremental decisions, sccache covers
  cross-tree recompiles.
- emsdk via setup-emsdk's built-in cache.

Timeout 90 minutes. No sccache-dist farm this round (single job, limited
gain, emsdk-clang distribution unproven — follow-up item).

## Job: `web-package` (needs: wasm-build)

1. pnpm/node setup identical to `ts.yml` (pnpm 11, node 24, lockfile cache)
2. `pnpm install --frozen-lockfile --filter '!electron-demo' --filter '!@anyfs/native'`
3. Download `anyfs-wasm-bundle` → `ts/packages/core/wasm/`
4. `pnpm -r build` the packages (core / react / trees)
5. Run the sync script (prerequisite 5): browser bundle files →
   `vite-demo/public/wasm/`
6. `vite build` → `tar -czf anyfs-web-dist.tar.gz -C ts/examples/vite-demo/dist .`
   (CI checkouts have no `public/disks/`, so no large images can leak in;
   `public/Caddyfile` rides along and feeds caddy directly)
7. Upload artifact **`anyfs-web-dist`**

## Job: `electron-package` (needs: web-package; matrix: linux-x64, win32-x64)

1. pnpm/node setup as above; this time `pnpm install` **includes**
   electron-demo (optionalDependencies fix); `@anyfs/native` stays filtered
   out so node-gyp never runs
2. Download `anyfs-web-dist` → `ts/examples/vite-demo/dist/` (renderer is not
   rebuilt)
3. `pnpm build:main` (esbuild) → `stage:renderer` → `package:wasm` /
   `package:win:wasm` (electron-packager, win32 cross-packaged from Linux)
4. Zip and upload **`anyfs-electron-linux-x64`** / **`anyfs-electron-win32-x64`**
   (wasm-only: no `.node`, no drivelist, no win DLL staging)
5. **Linux package smoke test:** launch the packaged binary under
   `xvfb-run`; alive after 30 s = pass, then kill. No win32 smoke (running
   electron under wine is not worth it in this chain).
6. `~/.cache/electron` in `actions/cache` (electron binary zips, linux +
   win32)

## Guardrails, boundaries, acceptance

Failure semantics:

- Any red job breaks the chain; downstream jobs do not run. The export gate
  blocks a drifted bundle before upload.
- Missing sccache/S3 secret or backend failure → bare compiles, never a
  build failure (linux.yml contract).
- An `actions/cache` miss affects duration only, never correctness.

Out of scope this round:

- PR triggers (`ts.yml` remains the PR gate)
- npm publish, GitHub releases, demo-site auto-deploy
- native-mode electron packages (waiting on a public drivelist fork + the
  utilityProcess refactor)
- win32 package smoke (wine), Playwright E2E in CI
- sccache-dist farm hookup; sccache fork whitelist patch for
  `-Xclang -iwithsysroot` (both recorded as follow-ups)

Acceptance criteria:

1. One `workflow_dispatch` run: all four jobs green.
2. Three artifact families downloadable: `anyfs-wasm-bundle`,
   `anyfs-web-dist`, `anyfs-electron-{linux,win32}-x64`.
3. The web tarball, served locally by caddy (bundled Caddyfile), opens a
   disk image in the browser.
4. The linux electron package launches under xvfb (CI smoke = job step 5).
5. A second push's run is significantly faster than the first (hard evidence
   that sccache + actions/cache are effective).

## Appendix: sccache × emcc research (2026-06-11)

Measured locally on emsdk 5.0.7 + sccache 0.10.0:

- sccache does not recognize `emcc` (Python driver). The supported hook is
  emscripten's `EM_COMPILER_WRAPPER` (`COMPILER_WRAPPER` config), which
  prefixes the *internal clang invocation* (`emcc.py:482`); upstream tests
  this exact setup with ccache.
- Plain `emcc -c`: **non-cacheable**. emcc injects
  `-Xclang -iwithsysroot/include/fakesdl` and `…/include/compat`
  (`tools/compile.py:136-139`), and sccache's clang parser rejects unknown
  `-Xclang` arguments (mozilla/sccache#955, #1266, #262; whitelist-patch
  precedent in #1603).
- `emcc -nostdinc -c`: **cacheable** (miss then hit). Kbuild always passes
  `-nostdinc`, so the real LKL `.cmd` compile command caches cleanly —
  verified miss→hit with the exact saved command.
- Unit economics: LKL = 1754 .o (≈80% of compile units, all cacheable
  out-of-the-box); QEMU = 447 .o (all rejected; the offending flags come
  solely from emcc's injection — meson's own argv is clean).
- Wall-clock note: even at 100% hit rate the emcc Python driver still runs
  per file (~100-200 ms × 1754 / parallelism), so warm builds bottom out
  around 10-15 min, not zero.
