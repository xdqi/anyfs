# Build System Refactor — P0+P1 Implementation Plan (Prune + Config + peru + doctor)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the foundation of the build-system refactor — delete dead/WinFSP code (zero behavior change to surviving targets), introduce the layered `build.config.toml` + gitignored `build.user.toml`, and make dependency fetching reproducible via `peru` + a `doctor` preflight — so the later phases (meson-orchestration, codegen, symbols) build on a clean, hardcode-free base.

**Architecture:** Two phases. **P0** prunes WinFSP (the only frontend not shipped — Windows ships core+server only) and other dead files, then adds a committed `build.config.toml` overlaid by a gitignored `build.user.toml`. **P1** replaces the scattered `$HOME`/`/opt`/CI-vs-script version pins with a single `peru.yaml` + `peru.lock` (the LKL/llvm forks captured this session are pinned here), a `doctor` preflight that validates tools/deps, and a CI `peru sync` step; a grep gate enforces no hardcoded `$HOME`/`/opt` remain in the build scripts.

**Tech Stack:** Meson/Ninja (native C build), Bash build scripts, [peru](https://github.com/buildinspace/peru) (Python, `pip install peru`) for dep fetch+lock, TOML config, `git`/`gh`.

**Design reference:** [docs/superpowers/specs/2026-05-31-build-system-refactor-design.md](../specs/2026-05-31-build-system-refactor-design.md)

---

## Scope

This is the **first** of the phased plans from the spec. It covers **P0 + P1 only**. Later phases get their own plans:
- **P2** — meson as orchestrator (`custom_target` graph) + bash dedup.
- **P3** — codegen (cross files, export lists) + GHA cache-manifest + `meson test`.
- **P4** — aggressive `ANYFS_EXPORT` + drift-gate over `.wasm`/`.node`.
- **P5** — optional: arm64/mingw32/wasm in CI; deeper Python absorption.

## Already done this session (do NOT redo — peru pins these)

- LKL port captured + unified: **`xdqi/lkl@lkl-anyfs`** (single ref; builds wasm + linux-amd64 + mingw64), component branches `xdqi/lkl@lkl618-{wasm,win64}`.
- Patched wasm-ld: **`xdqi/llvm-wasm@wasm-18.1.2-anyfs`** (LLD 18.1.2 + the `-r`/SECTIONS{} edits).
- wasm self-harvests `syscall_defs.h` (no mingw32 preseed); wasm config uses minimal NET — already committed in `gen_lkl_config_wasm.sh`.
- binutils 2.46 is NOT a separate dependency — it comes from msys2-cross (mingw) + system (native).

## File structure

**Delete (P0):**
- `cross-win32.txt`, `cross-win32-fuse.txt`, `cross-win64-fuse.txt` — legacy/dead cross files (active flow uses `scripts/cross-anyfs-{mingw32,mingw64}.txt`).
- `scripts/package_win32.sh` — legacy 32-bit packager (uses non-existent `builddir-win32/`, `/opt/msys2`).
- `scripts/mkwinfsp_implib.sh` — WinFSP import-lib generator.
- `mingw32-toolchain.cmake`, `mingw64-toolchain.cmake` — **only if Task 4's grep confirms no references**.

**Modify (P0):**
- `meson_options.txt` — remove `enable_winfsp` + `winfsp_root` options.
- `meson.build` — remove the `anyfs-winfsp` target + `enable_winfsp` block + winfsp lib selection.
- `src/fuse/fuse_main.c` — remove the `#ifdef _WIN32` WinFSP branches; keep the Linux libfuse3 path.
- `scripts/build_anyfs.sh` — remove the `--winfsp-root`, `anyfs-winfsp` component, and WinFSP DLL staging; the `fuse` component becomes Linux-only (mingw stays `core,server`).
- `.gitignore` — add `build.user.toml`.

**Create (P0):**
- `build.config.toml` — committed defaults.
- `build.user.toml.example` — documented template (the real `build.user.toml` is gitignored).

**Create (P1):**
- `peru.yaml`, `peru.lock` — pinned dependency manifest + lock.
- `scripts/lib/config.sh` — sources `build.config.toml` (+ `build.user.toml` override) into shell vars.
- `scripts/doctor.sh` — preflight tool/dep validation.

**Modify (P1):**
- `scripts/build_lkl.sh`, `scripts/gen_lkl_config.sh`, `scripts/build_anyfs.sh` — read paths from `scripts/lib/config.sh` instead of hardcoded `$HOME`/`/opt`.
- `.github/workflows/linux.yml`, `.github/workflows/mingw64.yml` — fetch deps via `peru sync` + run `doctor`.

---

## P0 — Prune + config skeleton

### Task 1: Remove WinFSP options from `meson_options.txt`

**Files:**
- Modify: `meson_options.txt`

- [ ] **Step 1: Confirm the current options exist**

Run: `grep -n 'enable_winfsp\|winfsp_root' meson_options.txt`
Expected: two `option(...)` blocks (`enable_winfsp` boolean, `winfsp_root` string).

- [ ] **Step 2: Delete both option blocks**

Remove the `option('enable_winfsp', ...)` and `option('winfsp_root', ...)` stanzas (4 lines + blank separators). Leave `enable_fuse` intact.

- [ ] **Step 3: Verify no references remain in options**

Run: `grep -c 'winfsp' meson_options.txt`
Expected: `0`

- [ ] **Step 4: Commit**

```bash
git add meson_options.txt
git commit -m "build(meson): drop enable_winfsp/winfsp_root options (WinFSP removed)"
```

### Task 2: Remove the `anyfs-winfsp` target from `meson.build`

**Files:**
- Modify: `meson.build`

- [ ] **Step 1: Locate the WinFSP block**

Run: `grep -n -i 'winfsp\|anyfs-winfsp' meson.build`
Expected: ~14 lines — the `enable_winfsp = get_option(...)` block, `winfsp_inc`, the `winfsp-x64`/`winfsp-x86` lib selection, and the `executable('anyfs-winfsp', ...)` target.

- [ ] **Step 2: Delete the whole `if enable_winfsp ... endif` block**

Remove from `enable_winfsp = get_option('enable_winfsp')` through the closing `endif` of the `anyfs-winfsp` executable. Do **not** touch the `enable_fuse` / `anyfs-fuse` (Linux libfuse3) block above it.

- [ ] **Step 3: Verify meson still configures for linux-amd64**

Run:
```bash
meson setup /tmp/b-lint --cross-file=/dev/null -Dlkl_dist=linux-amd64 -Dlkl_src=$HOME/linux 2>&1 | tail -5 || true
grep -ci winfsp meson.build
```
Expected: `grep` prints `0`. (If `meson setup` errors on unrelated missing deps that's fine for this lint — the point is no `winfsp` references and no parse error in the edited region.)

- [ ] **Step 4: Commit**

```bash
git add meson.build
git commit -m "build(meson): remove anyfs-winfsp target (Windows ships core+server only)"
```

### Task 3: Remove WinFSP branches from `src/fuse/fuse_main.c`

**Files:**
- Modify: `src/fuse/fuse_main.c`

- [ ] **Step 1: Find the `_WIN32` guards**

Run: `grep -n '_WIN32' src/fuse/fuse_main.c`
Expected: several `#ifdef _WIN32` / `#ifndef _WIN32` blocks (WinFSP type shims, the `fuse_parse_cmdline` divergence, the WinFSP mode-bit defines).

- [ ] **Step 2: Collapse each guard to the Linux path**

For each `#ifdef _WIN32 ... #else ... #endif`, keep the `#else` (Linux libfuse3) body and delete the `_WIN32` body + the `#ifdef`/`#else`/`#endif` lines. For each `#ifndef _WIN32 ... #endif`, keep the body, drop the guard lines. Delete the WinFSP-only `#define` shims entirely. The file must end up libfuse3-only.

- [ ] **Step 3: Verify no `_WIN32` remains**

Run: `grep -c '_WIN32' src/fuse/fuse_main.c`
Expected: `0`

- [ ] **Step 4: Build the Linux fuse frontend to prove no regression**

Run:
```bash
./scripts/build_anyfs.sh --targets=linux-amd64 --components=fuse -j$(nproc) 2>&1 | tail -15
ls -la build-anyfs-linux-amd64/bin/anyfs-fuse
```
Expected: build succeeds, `anyfs-fuse` exists.

- [ ] **Step 5: Commit**

```bash
git add src/fuse/fuse_main.c
git commit -m "fuse: drop WinFSP (_WIN32) code paths; libfuse3-only"
```

### Task 4: Delete dead files

**Files:**
- Delete: `cross-win32.txt`, `cross-win32-fuse.txt`, `cross-win64-fuse.txt`, `scripts/package_win32.sh`, `scripts/mkwinfsp_implib.sh`
- Delete (conditional): `mingw32-toolchain.cmake`, `mingw64-toolchain.cmake`

- [ ] **Step 1: Confirm the legacy cross/package/implib files are unreferenced**

Run:
```bash
git grep -l 'cross-win32\.txt\|cross-win32-fuse\|cross-win64-fuse\|package_win32\|mkwinfsp_implib' -- ':!docs' ':!*.md' || echo "NONE"
```
Expected: `NONE` (the active flow uses `scripts/cross-anyfs-{mingw32,mingw64}.txt`).

- [ ] **Step 2: Delete them**

```bash
git rm cross-win32.txt cross-win32-fuse.txt cross-win64-fuse.txt scripts/package_win32.sh scripts/mkwinfsp_implib.sh
```

- [ ] **Step 3: Decide the cmake toolchains**

Run: `git grep -l 'mingw32-toolchain.cmake\|mingw64-toolchain.cmake' -- ':!*.cmake' || echo "NONE"`
- If `NONE` → they are unused: `git rm mingw32-toolchain.cmake mingw64-toolchain.cmake`
- If any file references them (e.g. `ts/packages/anyfs-native/scripts/build-win64.sh`) → **keep them**; note in the commit message they're retained for the native-addon win64 cross-build.

- [ ] **Step 4: Verify the tree still builds linux-amd64 (sanity)**

Run: `./scripts/build_anyfs.sh --targets=linux-amd64 --components=core -j$(nproc) 2>&1 | tail -8`
Expected: succeeds.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore: delete dead cross files + package_win32.sh + mkwinfsp_implib.sh"
```

### Task 5: Remove WinFSP from `build_anyfs.sh`

**Files:**
- Modify: `scripts/build_anyfs.sh`

- [ ] **Step 1: Find the WinFSP handling**

Run: `grep -n -i 'winfsp\|anyfs-winfsp\|WINFSP_ROOT' scripts/build_anyfs.sh`
Expected: the `--winfsp-root` arg + default, the `anyfs-winfsp` component add for mingw, the `-Denable_winfsp=...` meson opts, and the WinFSP DLL staging.

- [ ] **Step 2: Delete the WinFSP arg, default, component, meson opts, and DLL staging**

Remove: the `WINFSP_ROOT=...` default + `--winfsp-root` arg parsing; the `mingw32|mingw64) out+=("anyfs-winfsp")` line; the whole `-Denable_winfsp` branch in the fuse-component section; the `winfsp` DLL `ln -sfn ...` staging lines. The `fuse` component is now Linux→`anyfs-fuse` only; for mingw the `fuse` component becomes a no-op (mingw builds `core,server`).

- [ ] **Step 3: Verify both targets still drive correctly**

Run:
```bash
grep -ci winfsp scripts/build_anyfs.sh
./scripts/build_anyfs.sh --targets=linux-amd64 --components=core,server,fuse -j$(nproc) 2>&1 | tail -8
./scripts/build_anyfs.sh --targets=mingw64 --components=core,server -j$(nproc) 2>&1 | tail -8
```
Expected: `grep` → `0`; both builds succeed (linux produces `anyfs-fuse`; mingw64 produces core+server, no fuse).

- [ ] **Step 4: Commit**

```bash
git add scripts/build_anyfs.sh
git commit -m "build: remove WinFSP/anyfs-winfsp from build_anyfs.sh"
```

### Task 6: Add the layered config skeleton

**Files:**
- Create: `build.config.toml`
- Create: `build.user.toml.example`
- Modify: `.gitignore`

- [ ] **Step 1: Write `build.config.toml` (committed defaults)**

```toml
# build.config.toml — committed, site-neutral defaults.
# Machine-local overrides go in build.user.toml (gitignored). Anything left as ""
# is discovered at build time by scripts/doctor.sh (which/pkg-config) where possible.

[paths]
# Where peru syncs source dependencies (relative to repo root).
deps_root   = "deps"
# Linux kernel / LKL source tree (peru: deps/linux, pinned to xdqi/lkl@lkl-anyfs).
linux_src   = ""            # "" => <deps_root>/linux
qemu_src    = ""            # "" => <deps_root>/qemu
util_linux  = ""            # "" => <deps_root>/util-linux
ksmbd_tools = ""            # "" => <deps_root>/ksmbd-tools

[toolchains]
emsdk        = ""           # "" => discover via $EMSDK or `which emcc`
msys2_cross  = "/opt/msys2-cross"   # mingw cross toolchain + sysroot + binutils 2.46
# Patched wasm-ld (built from xdqi/llvm-wasm). "" => <deps_root>/llvm-wasm/.../wasm-ld
wasm_ld      = ""
# Native binutils for the LKL kernel link (bypasses the stale tools/lkl/bin 2.25.1).
binutils_native = "/usr/bin"        # system binutils (>= 2.30); PE patch not needed for ELF

[build]
default_target = "linux-amd64"
jobs           = 0          # 0 => nproc
```

- [ ] **Step 2: Write `build.user.toml.example`**

```toml
# Copy to build.user.toml (gitignored) and override per machine.
# Only set what differs from build.config.toml.
# [paths]
# linux_src = "/home/me/linux"
# [toolchains]
# emsdk    = "/home/me/emsdk"
# wasm_ld  = "/home/me/llvm-wasm/install/bin/wasm-ld"
```

- [ ] **Step 3: Gitignore the local override (the one ignore entry this effort adds)**

Append to `.gitignore`:
```
# Machine-local build config override (see build.config.toml / build.user.toml.example)
build.user.toml
```

- [ ] **Step 4: Verify TOML parses**

Run: `python3 -c "import tomllib,sys; tomllib.load(open('build.config.toml','rb')); print('OK')"`
Expected: `OK`

- [ ] **Step 5: Commit**

```bash
git add build.config.toml build.user.toml.example .gitignore
git commit -m "build: add layered build.config.toml + gitignored build.user.toml"
```

---

## P1 — peru + doctor + config wiring

### Task 7: Add `peru.yaml` (pinned dependency manifest)

**Files:**
- Create: `peru.yaml`

- [ ] **Step 1: Install peru (record the requirement)**

Run: `pipx install peru || pip install --user peru; peru --version`
Expected: a version prints. (Add `peru` to the repo's dev-setup docs in a later step/plan.)

- [ ] **Step 2: Write `peru.yaml`**

```yaml
# peru.yaml — pinned, heterogeneous build dependencies. `peru sync` fetches into ./deps/.
# Run `peru reup` to refresh pins into peru.lock.
imports:
  linux:        deps/linux
  qemu:         deps/qemu
  util-linux:   deps/util-linux
  ksmbd-tools:  deps/ksmbd-tools
  llvm-wasm:    deps/llvm-wasm
  drivelist:    deps/drivelist-anyfs

git module linux:
  url: https://github.com/xdqi/lkl.git
  rev: lkl-anyfs          # unified LKL ref: builds wasm + linux-amd64 + mingw64

git module qemu:
  url: https://gitlab.com/qemu-project/qemu.git
  rev: v11.0.0

git module util-linux:
  url: https://github.com/util-linux/util-linux.git
  rev: v2.40

git module ksmbd-tools:
  url: https://github.com/cifsd-team/ksmbd-tools.git
  rev: master

git module llvm-wasm:
  url: https://github.com/xdqi/llvm-wasm.git
  rev: wasm-18.1.2-anyfs  # LLD 18.1.2 + the -r/SECTIONS{} edits; only wasm-ld is consumed

git module drivelist:
  url: https://github.com/xdqi/drivelist-anyfs.git
  rev: main
```

> NOTE: confirm each `url`/`rev` against the current CI (`.github/workflows/linux.yml` env block) and the existing local checkouts before locking. The OOT-fs drivers (ZFS/APFS/NTFS+) are staged by `oot_fs.sh` and stay there for now (they layer onto `deps/linux`); migrating them into peru is a P2 item.

- [ ] **Step 3: Lock the pins (resolve branches → SHAs)**

Run: `peru sync && peru reup && cat peru.lock | head -40`
Expected: `peru.lock` lists each module with a concrete resolved `rev` (SHA). Branch refs (`master`, `main`, `lkl-anyfs`) become SHAs.

- [ ] **Step 4: Verify the synced LKL is the unified branch**

Run: `git -C deps/linux log --oneline -1 && git -C deps/linux log --oneline | grep -c 'lkl/wasm:'`
Expected: tip is the `lkl-anyfs` merge commit; the `lkl/wasm:` port commits are present.

- [ ] **Step 5: Commit**

```bash
git add peru.yaml peru.lock
git commit -m "deps: pin build dependencies via peru (LKL=xdqi/lkl@lkl-anyfs, wasm-ld=xdqi/llvm-wasm)"
```

### Task 8: Add `scripts/lib/config.sh` (read config into shell vars)

**Files:**
- Create: `scripts/lib/config.sh`

- [ ] **Step 1: Write the config loader**

```bash
# scripts/lib/config.sh — source this to load build.config.toml (+ build.user.toml override)
# into ANYFS_* shell vars. Usage: source "$(dirname "$0")/lib/config.sh"
# Requires python3 (tomllib, 3.11+) — the build host already has it.
_anyfs_repo_root() { cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd; }

anyfs_load_config() {
    local root; root="$(_anyfs_repo_root)"
    # Emit `export ANYFS_<SECTION>_<KEY>=<value>` lines; user.toml overrides config.toml.
    eval "$(python3 - "$root" <<'PY'
import sys, tomllib, os
root = sys.argv[1]
def load(p):
    try:
        with open(p, "rb") as f: return tomllib.load(f)
    except FileNotFoundError: return {}
cfg = load(os.path.join(root, "build.config.toml"))
usr = load(os.path.join(root, "build.user.toml"))
def merge(a, b):
    for k, v in b.items():
        a[k] = merge(a.get(k, {}), v) if isinstance(v, dict) else v
    return a
cfg = merge(cfg, usr)
def emit(prefix, d):
    for k, v in d.items():
        if isinstance(v, dict): emit(f"{prefix}{k.upper()}_", v)
        else: print(f'export ANYFS_{prefix}{k.upper()}={shq(v)}')
import shlex
def shq(v): return shlex.quote(str(v))
emit("", cfg)
PY
)"
    # Resolve "" defaults relative to deps_root.
    local deps="${ANYFS_PATHS_DEPS_ROOT:-deps}"
    : "${ANYFS_PATHS_LINUX_SRC:=$root/$deps/linux}"
    : "${ANYFS_PATHS_QEMU_SRC:=$root/$deps/qemu}"
    : "${ANYFS_PATHS_UTIL_LINUX:=$root/$deps/util-linux}"
    : "${ANYFS_PATHS_KSMBD_TOOLS:=$root/$deps/ksmbd-tools}"
    : "${ANYFS_TOOLCHAINS_EMSDK:=${EMSDK:-}}"
    export ANYFS_PATHS_LINUX_SRC ANYFS_PATHS_QEMU_SRC ANYFS_PATHS_UTIL_LINUX \
           ANYFS_PATHS_KSMBD_TOOLS ANYFS_TOOLCHAINS_EMSDK
}
anyfs_load_config
```

- [ ] **Step 2: Verify it loads**

Run: `bash -c 'source scripts/lib/config.sh; echo "linux=$ANYFS_PATHS_LINUX_SRC msys2=$ANYFS_TOOLCHAINS_MSYS2_CROSS binutils=$ANYFS_TOOLCHAINS_BINUTILS_NATIVE"'`
Expected: prints resolved paths (linux → `<repo>/deps/linux`, msys2 → `/opt/msys2-cross`, binutils → `/usr/bin`).

- [ ] **Step 3: Commit**

```bash
git add scripts/lib/config.sh
git commit -m "build: scripts/lib/config.sh loads layered build config into shell vars"
```

### Task 9: Wire `gen_lkl_config.sh` + `build_lkl.sh` to the config (kill the worst hardcodes)

**Files:**
- Modify: `scripts/gen_lkl_config.sh`
- Modify: `scripts/build_lkl.sh`

- [ ] **Step 1: Replace the `$HOME`/`$HOME/binutils-gdb` defaults in `gen_lkl_config.sh`**

At the top (after the arg-defaults), source the config and use it. Replace:
```bash
LINUX_DIR="$HOME/linux"
BINUTILS_DIR="${BINUTILS_DIR:-$HOME/binutils-gdb/build-combined/install/bin}"
```
with:
```bash
source "$(dirname "$0")/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
# binutils: msys2-cross for mingw, system for native (no separate binutils-gdb build).
binutils_dir_for() {  # $1 = target
    case "$1" in
        mingw32|mingw64) echo "$ANYFS_TOOLCHAINS_MSYS2_CROSS/bin" ;;
        *)               echo "$ANYFS_TOOLCHAINS_BINUTILS_NATIVE" ;;
    esac
}
```
Then in `configure_target`, set `BINUTILS_DIR="$(binutils_dir_for "$target")"` per target (the `KOPT += "LD=${BINUTILS_DIR}/${CROSS}ld"` etc. lines stay as-is, now resolved per target).

- [ ] **Step 2: Replace the `$HOME` defaults in `build_lkl.sh`**

Replace `LINUX_DIR="$HOME/linux"` / `OUT_PARENT="$HOME/anyfs-reader"` with:
```bash
source "$(dirname "$0")/lib/config.sh"
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT_PARENT="${OUT_PARENT:-$(cd "$(dirname "$0")/.." && pwd)}"
```

- [ ] **Step 3: Verify mingw64 LKL still builds with the config-driven binutils**

Run:
```bash
# point deps/linux at the working tree for this check, or set LINUX_DIR explicitly:
LINUX_DIR=$HOME/linux ./scripts/gen_lkl_config.sh --targets=mingw64 2>&1 | tail -3
grep -E 'LD :=' lkl-mingw64/tools/lkl/Makefile.conf | head -1
LINUX_DIR=$HOME/linux ./scripts/build_lkl.sh --targets=mingw64 -j$(nproc) 2>&1 | tail -3
ls -la lkl-mingw64/tools/lkl/lib/liblkl.dll
```
Expected: `Makefile.conf` `LD :=` points at `/opt/msys2-cross/bin/x86_64-w64-mingw32-ld`; build green; `liblkl.dll` exists.

- [ ] **Step 4: Verify linux-amd64 LKL still builds**

Run: `LINUX_DIR=$HOME/linux ./scripts/gen_lkl_config.sh --targets=linux-amd64 && LINUX_DIR=$HOME/linux ./scripts/build_lkl.sh --targets=linux-amd64 -j$(nproc) 2>&1 | tail -3`
Expected: green; `lkl-linux-amd64/tools/lkl/liblkl.a` exists.

- [ ] **Step 5: Commit**

```bash
git add scripts/gen_lkl_config.sh scripts/build_lkl.sh
git commit -m "build: gen_lkl_config/build_lkl read paths+binutils from build config (drop \$HOME/binutils-gdb)"
```

### Task 10: Add `scripts/doctor.sh` (preflight validation)

**Files:**
- Create: `scripts/doctor.sh`

- [ ] **Step 1: Write the doctor**

```bash
#!/usr/bin/env bash
# scripts/doctor.sh — validate the toolchain/deps before a build. Exit non-zero on any failure.
set -uo pipefail
source "$(dirname "$0")/lib/config.sh"
fail=0
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=1; }

echo "== tools =="
command -v meson    >/dev/null && ok "meson"            || bad "meson not found"
command -v ninja    >/dev/null && ok "ninja"            || bad "ninja not found"
command -v peru     >/dev/null && ok "peru"             || bad "peru not found (pip install peru)"
command -v python3  >/dev/null && ok "python3"          || bad "python3 not found"

echo "== native binutils (>= 2.30 for kernel 6.13+) =="
v=$("$ANYFS_TOOLCHAINS_BINUTILS_NATIVE/ld" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
[ -n "$v" ] && awk "BEGIN{exit !($v >= 2.30)}" && ok "ld $v" || bad "native ld < 2.30 or missing ($ANYFS_TOOLCHAINS_BINUTILS_NATIVE/ld)"

echo "== mingw cross (msys2-cross, carries binutils 2.46 + PE weak-symbol patch) =="
mw="$ANYFS_TOOLCHAINS_MSYS2_CROSS/bin/x86_64-w64-mingw32-ld"
[ -x "$mw" ] && ok "$($mw --version | head -1)" || bad "mingw64 ld missing: $mw"

echo "== wasm-ld (patched, from xdqi/llvm-wasm — only this binary is consumed) =="
wl="${ANYFS_TOOLCHAINS_WASM_LD:-}"
[ -z "$wl" ] && wl="$ANYFS_PATHS_DEPS_ROOT/llvm-wasm/workspace/install/llvm/bin/wasm-ld"
if [ -x "$wl" ]; then
    ver=$("$wl" --version 2>/dev/null | grep -oE 'LLD [0-9]+' | head -1)
    [ "$ver" = "LLD 18" ] && ok "$ver ($wl)" || bad "wasm-ld is '$ver', expected LLD 18 ($wl)"
else
    bad "patched wasm-ld not built — run: (cd deps/llvm-wasm && ./linux-wasm.sh build-llvm)"
fi

echo "== deps synced + at locked SHA =="
if [ -d "$ANYFS_PATHS_LINUX_SRC/.git" ]; then
    ok "linux at $(git -C "$ANYFS_PATHS_LINUX_SRC" rev-parse --short HEAD)"
else
    bad "deps not synced — run: peru sync"
fi

[ "$fail" -eq 0 ] && echo "doctor: all checks passed" || echo "doctor: FAILURES above"
exit "$fail"
```

- [ ] **Step 2: Make it executable + run it**

Run: `chmod +x scripts/doctor.sh && ./scripts/doctor.sh; echo "exit=$?"`
Expected: prints a checklist; on a properly provisioned host `exit=0`. (If `deps/` isn't peru-synced yet, the deps check FAILs with the actionable `peru sync` hint — that's correct behavior.)

- [ ] **Step 3: Commit**

```bash
git add scripts/doctor.sh
git commit -m "build: scripts/doctor.sh preflight (tools, binutils>=2.30, msys2-cross, wasm-ld LLD18, synced deps)"
```

### Task 11: CI — fetch deps via `peru sync` + run `doctor`

**Files:**
- Modify: `.github/workflows/linux.yml`
- Modify: `.github/workflows/mingw64.yml`

- [ ] **Step 1: Replace the per-dep `git clone` steps with `peru sync`**

In each workflow, replace the `git clone --depth=1 --branch=$LKL_REF ...` / qemu / ksmbd clone steps with:
```yaml
      - name: Install peru
        run: pipx install peru
      - name: Sync pinned dependencies
        run: peru sync
      - name: Preflight (doctor)
        run: ./scripts/doctor.sh
```
Remove the now-unused `LKL_REF` / `QEMU_REF` / `KSMBD_REF` env entries (the pins live in `peru.lock`). Point subsequent build steps at `deps/<name>` (e.g. `--linux=deps/linux`) instead of `$HOME/<name>`.

- [ ] **Step 2: Update the cache key to `peru.lock`**

Change the dependency cache `key:` from `…${{ env.LKL_REF }}…` to hash `peru.lock`:
```yaml
          key: deps-${{ hashFiles('peru.lock') }}
          path: deps
```

- [ ] **Step 3: Validate the workflow locally**

Run: `./scripts/run_action_local.sh linux 2>&1 | tail -30 || true`
Expected: the `peru sync` + `doctor` steps run; build proceeds against `deps/linux`. (Under `act`, `actions/cache` is a no-op, so deps fetch fresh — that's expected.)

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/linux.yml .github/workflows/mingw64.yml
git commit -m "ci: fetch deps via peru sync + run doctor; cache keyed on peru.lock"
```

### Task 12: Hardcoding grep gate

**Files:**
- Create: `scripts/lint-no-hardcoded-paths.sh`
- Modify: `.github/workflows/linux.yml` (add the lint step)

- [ ] **Step 1: Write the gate**

```bash
#!/usr/bin/env bash
# Fail if build scripts still hardcode $HOME or /opt paths (use build.config.toml instead).
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# Scripts that have been migrated to config (extend this allowlist as P1 progresses).
migrated='scripts/gen_lkl_config.sh scripts/build_lkl.sh'
rc=0
for f in $migrated; do
    if grep -nE '\$HOME|/opt/msys2|/home/[a-z]+/' "$root/$f"; then
        echo "FAIL: hardcoded path in $f (use scripts/lib/config.sh)"; rc=1
    fi
done
[ "$rc" -eq 0 ] && echo "lint: no hardcoded paths in migrated scripts"
exit "$rc"
```

- [ ] **Step 2: Run it**

Run: `chmod +x scripts/lint-no-hardcoded-paths.sh && ./scripts/lint-no-hardcoded-paths.sh; echo "exit=$?"`
Expected: `exit=0` (Task 9 migrated those two scripts). If it fails, fix the offending line in the script it names.

- [ ] **Step 3: Add it as a CI step in `linux.yml`**

```yaml
      - name: Lint — no hardcoded paths in migrated build scripts
        run: ./scripts/lint-no-hardcoded-paths.sh
```

- [ ] **Step 4: Commit**

```bash
git add scripts/lint-no-hardcoded-paths.sh .github/workflows/linux.yml
git commit -m "ci: gate against hardcoded \$HOME/opt paths in migrated build scripts"
```

---

## Self-review

- **Spec coverage (P0+P1):** WinFSP removal (T1–T3,T5) ✓; dead files (T4) ✓; layered config skeleton + gitignored override (T6) ✓; peru manifest+lock with the captured LKL/llvm refs (T7) ✓; config-driven scripts dropping `$HOME`/binutils-gdb (T8,T9) ✓; doctor (T10) ✓; CI peru sync + cache-on-`peru.lock` (T11) ✓; hardcoding gate (T12) ✓. The lld-patch capture from the spec's P0 is already done (xdqi/llvm-wasm) — noted, not re-tasked.
- **Deferred (correctly, to later plans):** `targets.toml`/`features.toml` + generated cross files (P2/P3), OOT-fs into peru (P2), meson orchestration (P2), export drift-gate (P4).
- **Type/name consistency:** config keys (`ANYFS_PATHS_LINUX_SRC`, `ANYFS_TOOLCHAINS_MSYS2_CROSS`, `ANYFS_TOOLCHAINS_BINUTILS_NATIVE`, `ANYFS_TOOLCHAINS_WASM_LD`) are produced by `config.sh` (T8) and consumed identically by `doctor.sh` (T10) and the wired scripts (T9).
- **Verification gates** are real builds/greps, not unit tests — appropriate for a build-system change; each behavior-changing task rebuilds the affected target to prove zero regression.

## Open confirmations for the implementer (resolve in Task 7 / Task 9)

- The exact `url`/`rev` for `qemu`/`util-linux`/`ksmbd-tools`/`drivelist` — cross-check against `.github/workflows/linux.yml` env + existing local checkouts before `peru.lock` is committed.
- Whether `mingw{32,64}-toolchain.cmake` are referenced by `ts/packages/anyfs-native/scripts/build-win64.sh` (Task 4 Step 3 decides keep-vs-delete).
