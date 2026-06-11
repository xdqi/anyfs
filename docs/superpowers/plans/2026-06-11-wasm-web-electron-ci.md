# wasm → web → electron CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `.github/workflows/wasm.yml` that builds the wasm bundle (LKL + QEMU via emscripten), packages the vite-demo web app as a deployable tarball, and packages the electron demo (wasm mode) for linux-x64 + win32-x64 — one chained workflow run, artifacts only.

**Architecture:** Job chain `wasm-build → web-package → electron-package (matrix)` with artifact hand-off. sccache (local mode + SeaweedFS S3, engine from `xdqi/sccache-dist-action@v0.0.6` release assets) accelerates LKL/core emcc compiles via `EM_COMPILER_WRAPPER`; QEMU is covered by an `actions/cache`d build dir instead (emcc injects `-Xclang -iwithsysroot` into non-`-nostdinc` compiles, which sccache rejects).

**Tech Stack:** GitHub Actions, emscripten 5.0.7, peru, meson/ninja (QEMU), pnpm 11 / node 24 / vite, electron-packager.

**Spec:** `docs/superpowers/specs/2026-06-11-wasm-web-electron-ci-design.md`

**Branch:** work directly on `main` (repo convention; workflows only trigger from main anyway).

---

### Task 1: Commit the wasm patch sets

The wasm CI cannot run without `patches/linux/wasm/` (already referenced by `scripts/oot_fs.sh:81`) and `patches/qemu/` (consumed by the new `build_qemu_wasm.sh` in Task 2). Both are currently untracked.

**Files:**
- Commit (untracked → tracked): `patches/linux/wasm/series`, `patches/linux/wasm/01-xfs-this-address-wasm.patch`
- Commit (untracked → tracked): `patches/qemu/0001-thread-pool-inline-emscripten.patch` … `patches/qemu/0009-gitignore.patch` (9 files)

- [ ] **Step 1: Sanity-check the patch sets are complete and self-consistent**

Run: `cat patches/linux/wasm/series` — every line must name a file that exists in `patches/linux/wasm/`.
Run: `ls patches/qemu/*.patch | wc -l` — expected: `9`.
Run: `for p in patches/qemu/*.patch; do head -1 "$p" | grep -q '^diff --git' || echo "BAD: $p"; done` — expected: no output.

- [ ] **Step 2: Commit**

```bash
git add patches/linux patches/qemu
git commit -m "build(wasm): commit linux-wasm and qemu-emscripten patch sets

patches/linux/wasm: applied by oot_fs.sh stage (series file, idempotent
forward/reverse dry-run). patches/qemu: applied by the upcoming
build_qemu_wasm.sh; emscripten support for the QEMU block layer
(thread-pool inline, meson cross file, qemu-timer sleep, fdmon-poll,
oslib-posix, configure wasm32 cpu).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `scripts/build_qemu_wasm.sh`

Encapsulates what so far only exists in the local `~/qemu/build-anyfs-wasm/` build dir. The configure argv below was recovered from `~/qemu/build-anyfs-wasm/config.status` (initial run) plus the later reconfigure visible in `meson-logs/meson-log.txt` (`-Dbzip2=enabled -Dzstd=enabled`, zstd found via pkg-config at the wasm sysroot, `c = ['emcc','-m32']` in the generated cross file — the `-m32` comes from `--cpu=wasm32` via patch 0008).

**Files:**
- Create: `scripts/build_qemu_wasm.sh` (mode 755)

- [ ] **Step 1: Write the script**

```bash
#!/bin/bash
# Build the QEMU block layer for the wasm target (browser/node bundle).
#
# Applies patches/qemu/ (emscripten support) to $QEMU_ROOT idempotently,
# configures with the emscripten toolchain, and builds the static archives
# consumed by build_anyfs_wasm.sh Phase 3:
#   libblock.a libio.a libqom.a libauthz.a libcrypto.a
#   libevent-loop-base.a libqemuutil.a
#
# Inputs (env overrides beat build.config.toml via lib/config.sh):
#   QEMU_ROOT     QEMU source tree          (default: paths.qemu_src)
#   EMSDK_DIR     emsdk install root        (default: toolchains.emsdk / $EMSDK)
#   WASM_SYSROOT  wasm static-libs sysroot  (default: paths.wasm_sysroot)
#   BLD           build dir                 (default: $QEMU_ROOT/build-anyfs-wasm)
#
# Usage: ./build_qemu_wasm.sh [-j N]
set -euo pipefail

# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

QEMU_ROOT="${QEMU_ROOT:-$ANYFS_PATHS_QEMU_SRC}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"
SYS="${WASM_SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
BLD="${BLD:-$QEMU_ROOT/build-anyfs-wasm}"
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j)  JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        *)   echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -f "$QEMU_ROOT/configure" ]] || { echo "no QEMU tree at $QEMU_ROOT" >&2; exit 1; }
[[ -d "$SYS/lib" ]] || { echo "no wasm sysroot at $SYS (run fetch_wasm_sysroot.sh)" >&2; exit 1; }

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

# ── Apply patches/qemu (idempotent, same mechanism as oot_fs.sh) ──────
for p in "$REPO_ROOT"/patches/qemu/*.patch; do
    name="$(basename "$p")"
    if (cd "$QEMU_ROOT" && patch -p1 --dry-run --silent < "$p") >/dev/null 2>&1; then
        (cd "$QEMU_ROOT" && patch -p1 --silent < "$p")
        echo "applied qemu patch: $name"
    elif (cd "$QEMU_ROOT" && patch -p1 -R --dry-run --silent < "$p") >/dev/null 2>&1; then
        echo "qemu patch already applied: $name"
    else
        echo "qemu patch $name neither applies forward nor is already applied" >&2
        exit 1
    fi
done

# ── Configure (skipped when the build dir is already configured) ─────
if [[ ! -f "$BLD/config-host.mak" ]]; then
    mkdir -p "$BLD"
    # zstd resolves via pkg-config from the wasm sysroot; bz2 via
    # find_library through --extra-ldflags (-L$SYS/lib). The -s emcc link
    # flags live in configs/meson/emscripten.txt (patch 0007).
    (cd "$BLD" && \
        CC=emcc CXX=em++ AR=emar RANLIB=emranlib \
        NM="$EMSDK_DIR/upstream/bin/llvm-nm" \
        PKG_CONFIG_LIBDIR="$SYS/lib/pkgconfig" \
        "$QEMU_ROOT/configure" \
            --cpu=wasm32 --static --cross-prefix= \
            --disable-system --disable-user --disable-tools \
            --disable-guest-agent --disable-docs \
            --disable-gtk --disable-sdl --disable-opengl --disable-vnc \
            --disable-spice --disable-gnutls --disable-blkio --disable-numa \
            --disable-cap-ng --disable-seccomp --disable-libssh \
            --disable-curl --disable-rbd --disable-glusterfs --disable-vde \
            --disable-nettle --disable-gcrypt --disable-smartcard \
            --disable-usb-redir --disable-libudev --disable-fuse \
            --disable-libiscsi --disable-libnfs --disable-pixman \
            --disable-png --enable-bzip2 --enable-zstd \
            --disable-tcg --disable-tcg-interpreter \
            --target-list= \
            --extra-cflags="-I$SYS/include" \
            --extra-ldflags="-L$SYS/lib")
fi

# ── Build exactly the archives build_anyfs_wasm.sh consumes ──────────
ARCHIVES=(libblock.a libio.a libqom.a libauthz.a libcrypto.a
          libevent-loop-base.a libqemuutil.a)
ninja -C "$BLD" -j "$JOBS" "${ARCHIVES[@]}"

for a in "${ARCHIVES[@]}"; do
    [[ -f "$BLD/$a" ]] || { echo "missing built archive: $BLD/$a" >&2; exit 1; }
done
echo "OK: QEMU wasm archives in $BLD"
```

- [ ] **Step 2: Make executable, lint**

Run: `chmod +x scripts/build_qemu_wasm.sh && ./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh`
Expected: both lints pass (no `/home/kosaka` literals, shellcheck clean).

- [ ] **Step 3: Verify the already-applied detection against the local tree**

The local `~/qemu` tree has the patches in place, so every patch must hit the "already applied" branch:

Run: `BLD=/tmp/qemu-wasm-verify QEMU_ROOT=$HOME/qemu ./scripts/build_qemu_wasm.sh -j"$(nproc)" 2>&1 | tee /tmp/build_qemu_wasm.log | head -20`
Expected: nine `qemu patch already applied:` lines, then configure runs in `/tmp/qemu-wasm-verify`.

- [ ] **Step 4: Verify the full build completes and produces the 7 archives**

Wait for Step 3's command to finish (~10–20 min).
Expected: exit 0, final line `OK: QEMU wasm archives in /tmp/qemu-wasm-verify`.
Run: `ls -la /tmp/qemu-wasm-verify/*.a` — all 7 archives present, non-empty.

- [ ] **Step 5: Cross-check against the reference build**

Run: `for a in libblock.a libio.a libqom.a libauthz.a libcrypto.a libevent-loop-base.a libqemuutil.a; do printf '%s: ' "$a"; cmp -s <(emar t /tmp/qemu-wasm-verify/$a | sort) <(emar t $HOME/qemu/build-anyfs-wasm/$a | sort) && echo same-members || echo DIFFERS; done` (with emsdk env sourced).
Expected: all `same-members`. (Member-name parity is the gate; object contents may differ by path remapping.) If `libqemuutil.a` differs only by additions, inspect — anything the bundle links must not be missing.

- [ ] **Step 6: Clean up and commit**

```bash
rm -rf /tmp/qemu-wasm-verify /tmp/build_qemu_wasm.log
git add scripts/build_qemu_wasm.sh
git commit -m "feat(build): add build_qemu_wasm.sh — QEMU block layer for wasm

Freezes the configure argv recovered from the local build-anyfs-wasm
dir (config.status + meson-log reconfigure: bzip2/zstd enabled, zstd
via sysroot pkg-config). Applies patches/qemu idempotently with the
same forward/reverse dry-run mechanism as oot_fs.sh.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: `scripts/sync_wasm_bundle.sh`

Replaces the manual copy of the built bundle into vite-demo's public dir. Only the three files the app actually loads (`App.tsx` references `/wasm/anyfs.worker.js`; `public/anyfs-worker.js` imports `anyfs.mjs`; the `anyfs.workeronly.*` files in both dirs are stale experiments — do NOT copy them).

**Files:**
- Create: `scripts/sync_wasm_bundle.sh` (mode 755)

- [ ] **Step 1: Write the script**

```bash
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
    cp -f "$src/$f" "$dst/$f"
done
echo "synced anyfs.{mjs,wasm,worker.js} -> $dst"
```

- [ ] **Step 2: Make executable, lint, run locally**

Run: `chmod +x scripts/sync_wasm_bundle.sh && ./scripts/lint-shellcheck.sh && ./scripts/sync_wasm_bundle.sh`
Expected: lint clean; `synced anyfs.{mjs,wasm,worker.js} -> …/ts/examples/vite-demo/public/wasm`.
Run: `cmp ts/packages/core/wasm/anyfs.wasm ts/examples/vite-demo/public/wasm/anyfs.wasm && echo identical`
Expected: `identical`.

- [ ] **Step 3: Commit**

```bash
git add scripts/sync_wasm_bundle.sh
git commit -m "feat(build): sync_wasm_bundle.sh — copy core wasm bundle into vite-demo public/

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Smoke-test fixture generator

`ts/packages/core/test/smoke.node.mjs single` expects a whole-disk ext4 image at `ts/examples/vite-demo/public/disks/single.img` and mounts it whole (`mountWhole: 'ext4'`, `smoke.node.mjs:115`) — any populated ext4 image works. `mkfs.ext4 -d` populates without root.

**Files:**
- Create: `ts/packages/core/test/make-single-image.sh` (mode 755)

- [ ] **Step 1: Write the generator**

```bash
#!/usr/bin/env bash
# Generate a minimal whole-disk ext4 image for `smoke.node.mjs single`.
# mkfs.ext4 -d populates the fs from a directory without root.
# Usage: make-single-image.sh <out.img>
set -euo pipefail
out="${1:?usage: make-single-image.sh <out.img>}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "hello from the anyfs smoke fixture" > "$tmp/hello.txt"
mkdir -p "$tmp/subdir"
head -c 1048576 /dev/urandom > "$tmp/subdir/random-1mib.bin"

mkdir -p "$(dirname "$out")"
rm -f "$out"
truncate -s 64M "$out"
mkfs.ext4 -q -F -d "$tmp" "$out"
echo "wrote $out"
```

- [ ] **Step 2: Lint + verify locally against the node bundle**

The local `single.img` is a symlink to an external image — park it, generate, run the smoke, restore:

```bash
chmod +x ts/packages/core/test/make-single-image.sh
./scripts/lint-shellcheck.sh
cd ts/examples/vite-demo/public/disks
mv single.img single.img.bak
bash ../../../../packages/core/test/make-single-image.sh single.img
cd ../../../../packages/core
node test/smoke.node.mjs single
cd ../../examples/vite-demo/public/disks
rm single.img && mv single.img.bak single.img
```

Expected: smoke prints partition JSON, enters the fs, readdir/pread succeed, exit 0. (If `mkfs.ext4` features trip the LKL mount, retry with `-O ^metadata_csum_seed,^orphan_file` added to the `mkfs.ext4` line in the script — older guest ext4 sometimes rejects newest defaults — and keep whichever variant passes.)

- [ ] **Step 3: Commit**

```bash
git add ts/packages/core/test/make-single-image.sh
git commit -m "test(core): generator for the smoke single.img ext4 fixture

mkfs.ext4 -d — rootless, deterministic enough for CI; unblocks running
smoke.node.mjs single on bare runners.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: electron-demo wasm-only packaging

Two changes: (a) `drivelist` + `@anyfs/native` become `optionalDependencies` so `pnpm install` survives a bare runner where `file:../../../../drivelist-anyfs` doesn't exist (`main.ts` already lazy-loads both behind try/catch with wasm fallback — `loadNativeAddon()` at `main.ts:459` warns and returns null); (b) `package:wasm` / `package:win:wasm` scripts that skip native staging and do NOT rebuild the renderer.

**Files:**
- Modify: `ts/examples/electron-demo/package.json`
- Modify (regenerated): `ts/pnpm-lock.yaml`

- [ ] **Step 1: Move the two deps to optionalDependencies**

In `ts/examples/electron-demo/package.json` replace:

```json
    "dependencies": {
        "@anyfs/native": "workspace:*",
        "drivelist": "file:../../../../drivelist-anyfs"
    },
```

with:

```json
    "optionalDependencies": {
        "@anyfs/native": "workspace:*",
        "drivelist": "file:../../../../drivelist-anyfs"
    },
```

- [ ] **Step 2: Add the wasm-only package scripts**

Add to `"scripts"` in the same file (the argv is the existing `package`/`package:win` minus `pnpm build` → `pnpm build:main`, minus every `stage-native*`/`copy-win64-dlls` step):

```json
        "package:wasm": "pnpm build:main && pnpm stage:renderer && mkdir -p $HOME/.cache/electron-packager-tmp && TMPDIR=$HOME/.cache/electron-packager-tmp electron-packager . anyfs-demo --platform=linux --arch=x64 --out=out --overwrite --prune=false --extra-resource=staging/renderer --ignore='^/(staging|out|src|esbuild\\.main\\.mjs|tsconfig\\.json|README\\.md|\\.gitignore|scripts|node_modules)($|/)'",
        "package:win:wasm": "pnpm build:main && pnpm stage:renderer && mkdir -p $HOME/.cache/electron-packager-tmp && TMPDIR=$HOME/.cache/electron-packager-tmp electron-packager . anyfs-demo --platform=win32 --arch=x64 --out=out --overwrite --prune=false --extra-resource=staging/renderer --ignore='^/(staging|out|src|esbuild\\.main\\.mjs|tsconfig\\.json|README\\.md|\\.gitignore|scripts|node_modules)($|/)'",
```

- [ ] **Step 3: Refresh the lockfile**

Run: `cd ts && pnpm install`
Expected: exit 0; `git status --short` shows `ts/pnpm-lock.yaml` modified (deps re-classified as optional).

- [ ] **Step 4: Verify wasm-only linux packaging locally**

The renderer dist already exists locally (or build it: `pnpm --filter vite-demo build`).

Run: `cd ts/examples/electron-demo && pnpm run package:wasm`
Expected: exit 0, `out/anyfs-demo-linux-x64/anyfs-demo` exists.
Run: `find out/anyfs-demo-linux-x64 -name '*.node' | wc -l`
Expected: `0` (no native addon staged).
Run: `ls out/anyfs-demo-linux-x64/resources/renderer/index.html`
Expected: present (renderer staged as extra-resource).

- [ ] **Step 5: Verify the packaged app still launches (wasm fallback path)**

Run: `cd ts/examples/electron-demo && (./out/anyfs-demo-linux-x64/anyfs-demo --no-sandbox & APP=$!; sleep 15; kill -0 $APP && echo ALIVE; kill $APP)`
Expected: `ALIVE` (the `[anyfs-native] addon not loadable` warning in stderr is the designed fallback, not an error).

- [ ] **Step 6: Commit**

```bash
git add ts/examples/electron-demo/package.json ts/pnpm-lock.yaml
git commit -m "feat(electron-demo): wasm-only packaging + optional native deps

drivelist/@anyfs/native move to optionalDependencies (main.ts already
lazy-loads both with wasm fallback) so pnpm install works on bare CI
runners without the sibling drivelist checkout. package:wasm and
package:win:wasm skip native staging and consume a prebuilt renderer.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: `.github/workflows/wasm.yml`

**Files:**
- Create: `.github/workflows/wasm.yml`

- [ ] **Step 1: Write the workflow**

```yaml
name: wasm

# Builds the wasm bundle (LKL + QEMU block layer via emscripten), packages
# the vite-demo web app as a deployable tarball, and packages the electron
# demo (wasm mode only) for linux-x64 + win32-x64.
# Design: docs/superpowers/specs/2026-06-11-wasm-web-electron-ci-design.md
#
# Chain:     wasm-build → web-package → electron-package (matrix)
# Artifacts: anyfs-wasm-bundle, anyfs-web-dist, anyfs-electron-{linux,win32}-x64
#
# sccache: local mode + SeaweedFS S3 (secrets SCCACHE_S3_*); engine binary
# from the xdqi/sccache-dist-action v0.0.6 release. EM_COMPILER_WRAPPER
# hooks sccache onto emcc's internal clang calls — covers the 1754 LKL
# kernel units + core compiles. QEMU is NOT routed through sccache (emcc
# injects -Xclang -iwithsysroot into non--nostdinc compiles and sccache's
# clang parser rejects those; mozilla/sccache#955); its build dir is
# actions/cache'd instead. Missing secrets degrade to bare compiles —
# the cache must never break the build (same contract as linux.yml).

on:
  push:
    branches: [main]
  workflow_dispatch:

concurrency:
  group: wasm-${{ github.ref }}
  cancel-in-progress: true

jobs:
  wasm-build:
    runs-on: ubuntu-24.04
    timeout-minutes: 90
    env:
      # secrets are not allowed in `if:` — launder through env.
      S3_SECRET_SET: ${{ secrets.SCCACHE_S3_BUCKET != '' }}
    steps:
      - uses: actions/checkout@v4

      - name: Install build dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            build-essential meson ninja-build pkg-config python3 pipx \
            flex bison bc rsync e2fsprogs \
            curl ca-certificates xz-utils

      # Same key as linux.yml's deps cache — identical content, shared hit.
      # oot_fs.sh stage is idempotent against a post-stage cached tree.
      - name: Cache peru-synced deps
        id: cache-deps
        uses: actions/cache@v4
        with:
          path: deps
          key: deps-${{ hashFiles('peru.yaml') }}

      - name: Cache LKL wasm build tree
        uses: actions/cache@v4
        with:
          path: lkl-wasm
          key: lkl-wasm-${{ hashFiles('peru.yaml', 'scripts/gen_lkl_config_wasm.sh', 'scripts/build_lkl_wasm.sh', 'scripts/oot_fs.sh', 'patches/linux/wasm/**', '.github/workflows/wasm.yml') }}
          restore-keys: |
            lkl-wasm-

      - name: Cache OOT FS sources (NTFS PLUS + APFS + OpenZFS)
        id: cache-oot
        uses: actions/cache@v4
        with:
          path: ~/oot-fs
          key: oot-fs-${{ hashFiles('scripts/oot_fs.sh') }}
          restore-keys: |
            oot-fs-

      - name: Cache QEMU wasm build
        uses: actions/cache@v4
        with:
          path: deps/qemu/build-anyfs-wasm
          key: qemu-wasm-${{ hashFiles('peru.yaml', 'scripts/build_qemu_wasm.sh', 'patches/qemu/**') }}
          restore-keys: |
            qemu-wasm-

      - name: Install peru
        run: |
          pipx install peru
          echo "$HOME/.local/bin" >> "$GITHUB_PATH"

      - name: Sync pinned dependencies
        if: steps.cache-deps.outputs.cache-hit != 'true'
        run: peru sync

      # Pinned to the emcc the bundle was validated with (same as
      # wasm-sysroot.yml). Bump deliberately and re-validate.
      - name: Set up emsdk
        uses: mymindstorm/setup-emsdk@v14
        with:
          version: 5.0.7

      # config.sh auto-prefers .toolchain/wasm-ld + .toolchain/wasm-sysroot,
      # and ANYFS_TOOLCHAINS_EMSDK falls back to $EMSDK (set by setup-emsdk).
      - name: Fetch patched wasm-ld + wasm sysroot
        run: |
          ./scripts/fetch_wasm_ld.sh
          ./scripts/fetch_wasm_sysroot.sh

      - name: Fetch OOT FS sources
        if: steps.cache-oot.outputs.cache-hit != 'true'
        run: ./scripts/oot_fs.sh fetch

      - name: Configure sccache (local mode + S3)
        if: ${{ env.S3_SECRET_SET == 'true' }}
        continue-on-error: true
        env:
          SCCACHE_BUCKET: ${{ secrets.SCCACHE_S3_BUCKET }}
          SCCACHE_ENDPOINT: ${{ secrets.SCCACHE_S3_ENDPOINT }}
          AWS_ACCESS_KEY_ID: ${{ secrets.SCCACHE_S3_KEY_ID }}
          AWS_SECRET_ACCESS_KEY: ${{ secrets.SCCACHE_S3_SECRET }}
          GH_TOKEN: ${{ github.token }}
        run: |
          mkdir -p "$HOME/.local/bin"
          gh release download v0.0.6 -R xdqi/sccache-dist-action \
            -p 'sccache-sccache-dist-poc-tweaks-linux-amd64' \
            -O "$HOME/.local/bin/sccache"
          chmod +x "$HOME/.local/bin/sccache"
          {
            echo "SCCACHE_BUCKET=${SCCACHE_BUCKET}"
            echo "SCCACHE_ENDPOINT=${SCCACHE_ENDPOINT}"
            echo "AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID}"
            echo "AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY}"
            echo "SCCACHE_REGION=auto"
            echo "SCCACHE_S3_USE_SSL=true"
            echo "SCCACHE_S3_KEY_PREFIX=anyfs-wasm"
            echo "SCCACHE_IDLE_TIMEOUT=0"
            echo "EM_COMPILER_WRAPPER=sccache"
          } >> "$GITHUB_ENV"
          sccache --start-server
          sccache --show-stats

      - name: Generate LKL wasm kernel config
        run: ./scripts/gen_lkl_config_wasm.sh --linux=deps/linux --out="$GITHUB_WORKSPACE"

      - name: Build LKL (wasm)
        run: ./scripts/build_lkl_wasm.sh --linux=deps/linux --out="$GITHUB_WORKSPACE" -j "$(nproc)"

      # QEMU compiles are not sccache-cacheable (see header) — unset the
      # wrapper so 447 guaranteed-miss requests don't slow the build.
      - name: Build QEMU block layer (wasm)
        env:
          EM_COMPILER_WRAPPER: ''
        run: QEMU_ROOT=deps/qemu ./scripts/build_qemu_wasm.sh -j "$(nproc)"

      - name: Build anyfs wasm bundle (browser + node)
        env:
          LINUX_DIR: deps/linux
          QEMU_ROOT: deps/qemu
        run: |
          ./scripts/build_anyfs_wasm.sh
          ANYFS_TARGET=node ./scripts/build_anyfs_wasm.sh

      - name: sccache stats
        if: ${{ env.S3_SECRET_SET == 'true' }}
        continue-on-error: true
        run: sccache --show-stats || true

      - name: Wasm export drift gate
        run: ./tests/test_wasm_exports.sh

      - name: Smoke test (node bundle, generated ext4 image)
        run: |
          bash ts/packages/core/test/make-single-image.sh \
            ts/examples/vite-demo/public/disks/single.img
          cd ts/packages/core && node test/smoke.node.mjs single

      - name: Upload wasm bundle
        uses: actions/upload-artifact@v4
        with:
          name: anyfs-wasm-bundle
          path: ts/packages/core/wasm/
          if-no-files-found: error

  web-package:
    needs: wasm-build
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@v4
      - uses: pnpm/action-setup@v4
        with:
          version: 11
          package_json_file: ts/package.json
      - uses: actions/setup-node@v4
        with:
          node-version: 24
          cache: pnpm
          cache-dependency-path: ts/pnpm-lock.yaml
      - name: Install
        working-directory: ts
        run: pnpm install --frozen-lockfile --filter '!electron-demo' --filter '!@anyfs/native'
      - name: Download wasm bundle
        uses: actions/download-artifact@v4
        with:
          name: anyfs-wasm-bundle
          path: ts/packages/core/wasm
      - name: Build packages
        working-directory: ts
        run: pnpm -r --filter './packages/*' --filter '!@anyfs/native' build
      - name: Sync bundle into vite-demo public/
        run: ./scripts/sync_wasm_bundle.sh
      - name: Build vite-demo
        working-directory: ts
        run: pnpm --filter vite-demo build
      # CI checkouts have no public/disks (gitignored locally), so nothing
      # to prune; the bundled Caddyfile feeds caddy directly on deploy.
      - name: Pack web dist
        run: tar -czf anyfs-web-dist.tar.gz -C ts/examples/vite-demo/dist .
      - name: Upload web dist
        uses: actions/upload-artifact@v4
        with:
          name: anyfs-web-dist
          path: anyfs-web-dist.tar.gz
          if-no-files-found: error

  electron-package:
    needs: web-package
    runs-on: ubuntu-24.04
    timeout-minutes: 30
    strategy:
      fail-fast: false
      matrix:
        include:
          - platform: linux
            script: package:wasm
            outdir: anyfs-demo-linux-x64
          - platform: win32
            script: package:win:wasm
            outdir: anyfs-demo-win32-x64
    steps:
      - uses: actions/checkout@v4
      - uses: pnpm/action-setup@v4
        with:
          version: 11
          package_json_file: ts/package.json
      - uses: actions/setup-node@v4
        with:
          node-version: 24
          cache: pnpm
          cache-dependency-path: ts/pnpm-lock.yaml
      - name: Cache electron binaries
        uses: actions/cache@v4
        with:
          path: ~/.cache/electron
          key: electron-${{ matrix.platform }}-${{ hashFiles('ts/pnpm-lock.yaml') }}
      # Includes electron-demo this time. drivelist/@anyfs/native are
      # optionalDependencies: the missing sibling drivelist checkout is
      # skipped, and the @anyfs/native filter keeps node-gyp from running.
      - name: Install
        working-directory: ts
        run: pnpm install --frozen-lockfile --filter '!@anyfs/native'
      - name: Download web dist
        uses: actions/download-artifact@v4
        with:
          name: anyfs-web-dist
          path: .
      - name: Unpack renderer
        run: |
          mkdir -p ts/examples/vite-demo/dist
          tar -xzf anyfs-web-dist.tar.gz -C ts/examples/vite-demo/dist
      - name: Package (${{ matrix.platform }}, wasm-only)
        working-directory: ts/examples/electron-demo
        run: pnpm run ${{ matrix.script }}
      - name: Smoke test packaged app (linux)
        if: matrix.platform == 'linux'
        run: |
          sudo apt-get install -y --no-install-recommends xvfb libnss3 libgbm1
          xvfb-run -a ts/examples/electron-demo/out/anyfs-demo-linux-x64/anyfs-demo \
            --no-sandbox --disable-gpu &
          APP=$!
          sleep 30
          kill -0 "$APP"   # still alive after 30s = launch smoke passes
          kill "$APP" || true
          pkill -f anyfs-demo || true
      - name: Zip package
        run: |
          cd ts/examples/electron-demo/out
          zip -qry "$GITHUB_WORKSPACE/anyfs-electron-${{ matrix.platform }}-x64.zip" \
            '${{ matrix.outdir }}'
      - name: Upload electron package
        uses: actions/upload-artifact@v4
        with:
          name: anyfs-electron-${{ matrix.platform }}-x64
          path: anyfs-electron-${{ matrix.platform }}-x64.zip
          if-no-files-found: error
```

- [ ] **Step 2: Validate YAML + lint**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/wasm.yml'))" && echo YAML-OK`
Expected: `YAML-OK`. (If PyYAML is absent: `pipx run --spec pyyaml python` equivalent or `ruby -ryaml -e "YAML.load_file('.github/workflows/wasm.yml')"`.)
Also run `actionlint` if available (`command -v actionlint`); fix anything it reports.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/wasm.yml
git commit -m "ci(wasm): wasm-build → web-package → electron-package chain

LKL+QEMU emscripten build with sccache (local+S3, engine from
sccache-dist-action v0.0.6) on the LKL/core compiles, actions/cache on
the QEMU build dir; vite-demo deploy tarball; electron wasm-only
packages for linux-x64 + win32-x64 with a linux xvfb launch smoke.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Land, configure secrets, dispatch to green

- [ ] **Step 1: Push**

Run: `git push origin main`

- [ ] **Step 2: Configure the S3 secrets (USER ACTION — same values as msys2-cross)**

The repo currently has only `TS_OAUTH_SECRET`. Ask the user (kosaka) to run, with the SeaweedFS values used by msys2-cross:

```bash
gh secret set SCCACHE_S3_BUCKET   -R xdqi/anyfs
gh secret set SCCACHE_S3_ENDPOINT -R xdqi/anyfs
gh secret set SCCACHE_S3_KEY_ID   -R xdqi/anyfs
gh secret set SCCACHE_S3_SECRET   -R xdqi/anyfs
```

The workflow is green without them (bare compiles) — do not block on this step; revisit acceptance criterion 5 after they land.

- [ ] **Step 3: Dispatch and watch**

Run: `gh workflow run wasm -R xdqi/anyfs && sleep 10 && gh run list -R xdqi/anyfs --workflow=wasm --limit 1`
Then: `gh run watch -R xdqi/anyfs <run-id> --exit-status` (long; check back periodically).

Likely first-run failure points, in order, with the known fix direction:
- `pnpm install` in electron-package still failing on the missing drivelist path despite optionalDependencies → add a stub step before install: `mkdir -p ../drivelist-anyfs && echo '{"name":"drivelist","version":"0.0.0-stub","os":["!any"]}' > ../drivelist-anyfs/package.json` (placed so the relative `file:` path resolves from `ts/examples/electron-demo`); adjust until install passes.
- QEMU configure can't find bz2 → add an `embuilder build bzip2 zlib` step after emsdk setup (builds the emscripten ports the local tree had cached).
- meson too old for QEMU v11 on ubuntu-24.04 → `pipx install meson` (matches the 1.7.0 used locally).
- `smoke.node.mjs single` mount failure → apply the `mkfs.ext4 -O` fallback from Task 4 Step 2.

- [ ] **Step 4: Verify acceptance criteria 1–4**

1. All four jobs green in one dispatch.
2. `gh run download <run-id> -R xdqi/anyfs` — artifacts `anyfs-wasm-bundle`, `anyfs-web-dist`, `anyfs-electron-linux-x64`, `anyfs-electron-win32-x64` all present.
3. Web tarball serves: `mkdir /tmp/webdist && tar xzf anyfs-web-dist.tar.gz -C /tmp/webdist && caddy run --config /tmp/webdist/Caddyfile` (adjust root to /tmp/webdist), open the page, open a disk image.
4. Electron linux zip: unzip, `xvfb-run -a ./anyfs-demo --no-sandbox` stays alive 30 s.

- [ ] **Step 5: Verify acceptance criterion 5 (after secrets land)**

Trigger a second run (`gh workflow run wasm`), compare `wasm-build` durations and the `sccache stats` step (expect a high hit rate on the ~1754 LKL units and a visibly shorter job).

- [ ] **Step 6: Record follow-ups**

Append to the spec's out-of-scope list if anything was discovered during landing; confirm these two are still tracked there: sccache fork `-Xclang -iwithsysroot` whitelist (unlocks QEMU's 447 units), sccache-dist farm for the wasm job.

---

## Self-review notes

- **Spec coverage:** §architecture → Task 6; §prereq 1 → Task 1; §prereq 2 → Task 2; §prereq 3+4 → Task 5; §prereq 5 → Task 3; §wasm-build/web/electron jobs → Task 6; smoke stretch → Task 4 + wasm-build step; §guardrails (degrade-without-secrets, concurrency, timeouts) → Task 6 YAML; §acceptance → Task 7.
- **Known risk consciously accepted:** pnpm's tolerance of a missing `file:` optionalDependency is unverified until CI runs — Task 7 Step 3 carries the stub-package fallback.
- **The two `EM_COMPILER_WRAPPER` consumers** (build_lkl_wasm.sh, build_anyfs_wasm.sh) need no script changes — emcc reads the env var itself; the QEMU step explicitly clears it.
