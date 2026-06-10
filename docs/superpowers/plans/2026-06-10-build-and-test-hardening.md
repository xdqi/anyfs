# Build & Test Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the P1 build-system refactor (all build scripts read `build.config.toml`, shellcheck-clean), make the wasm export table drift-proof and single-sourced, give the patched wasm-ld and the wasm-sysroot reproducible release pipelines (mingw-style prebuilt provisioning), extract the shared daemon skeleton out of ksmbd/nfsd mains, add real C and TS unit-test suites wired into CI plus a triage of the legacy `tests/*.mjs` suites, and hook the linux + mingw64 CI jobs up to the sccache-dist compile farm.

**Architecture:** Seven phases, each shippable on its own, in the order the user specified: (1) config.sh migration for the 7 remaining hand-rolled scripts, with a shellcheck gate; (2) wasm export list generated from `ts/native/anyfs_ts.c` (single source of truth) + delete the bit-rotted `build_anyfs_browser_wasm.sh`; (2A) wasm-ld release pipeline on the `xdqi/llvm-wasm` fork (built on a sccache-dist farm) + fetch-script consumption in anyfs-reader; (2B) wasm-sysroot brought to the mingw provisioning model — in-repo recipe script + CI-published tarball + fetch + doctor manifest check; (3) new `src/server_common/` linked into both servers; (4) meson-registered C unit tests + node:test/vitest TS unit tests + CI wiring + triage of the legacy `tests/*.mjs` zoo (CDP↔Playwright parity, keep-don't-delete diagnostics); (5) `xdqi/sccache-dist-action` coordinator/worker jobs in `linux.yml` **and** (experimental) `mingw64.yml`, with graceful fallback when the farm or secret is absent.

**Tech Stack:** bash + shellcheck, meson/ninja, emscripten, node:test (Node ≥22), vitest + @testing-library/react + jsdom, GitHub Actions, sccache-dist, GitHub releases as artifact channel.

**Branch:** Start from `main` (NOT the current `zig-oldglibc-aligned-new` checkout): `git checkout main && git checkout -b build-and-test-hardening`. Use a worktree if executing with superpowers:using-git-worktrees. Phase 2A additionally works in a clone of `xdqi/llvm-wasm`.

**Out of scope (decided):** repo-root junk cleanup / committing `patches/` (separate hygiene pass), `fuse_main.c` decomposition and fuse adoption of server_common (libfuse owns its own signal handling), @anyfs/trees component tests (covered by Playwright E2E), packaging wasm sysroot libs into the msys-cross pacman repo (possible future unification, msys2-cross project's call).

**Shellcheck rule (applies to every task that touches a `scripts/*.sh` file):** the verification step must also run `shellcheck -x scripts/<file>` and fix (or explicitly `# shellcheck disable=`-annotate with a reason) every finding of severity warning or higher. shellcheck is installed locally.

**Key facts the executor must know (verified 2026-06-10):**
- `scripts/lib/config.sh` loads `build.config.toml` + `build.user.toml` into `ANYFS_*` vars (e.g. `ANYFS_PATHS_LINUX_SRC`, `ANYFS_TOOLCHAINS_MSYS2_CROSS`, `ANYFS_TOOLCHAINS_EMSDK`) and materializes `""` defaults to `<repo>/deps/...`.
- `scripts/lint-no-hardcoded-paths.sh` greps migrated scripts for `\$HOME|/opt/msys2|/home/[a-z]+/` — **including comments**, so doc comments mentioning `$HOME` must be reworded during migration.
- `scripts/build_anyfs_browser_wasm.sh` is **dead**: it compiles `src/core/anyfs.c`, `src/core/kindprobe.c`, `src/core/qemu_blk_backend.c` — none exist (renamed to `anyfs_kernel.c`/`anyfs_probe.c`/`qemu_backend.c`), and its `EXPORTED_FUNCS` uses pre-refactor `anyfs_ts_disk_*` names. It is untracked (never committed). The live script is `scripts/build_anyfs_wasm.sh` (QEMU always on, outputs `ts/packages/core/wasm/anyfs.mjs`).
- `build_anyfs.sh` must keep `LKL_SRC`/`QEMU_ROOT`/`KSMBD_ROOT` **relative** when inside the source tree (meson rejects absolute paths into the source root — see the NOTE at scripts/build_anyfs.sh:77).
- `anyfs_path_parse` rejects `p0` (`parse_component` requires `v != 0`), accepts `P1`, strips leading/trailing `/`, rejects bare `disk0`, max 8 components.
- The native bridge global is `globalThis.anyfsNative` (ts/packages/core/src/native-session.ts:32).
- Existing meson test executables (`test_raw_mount` etc.) are built but **never registered with `test()`** — there is no meson test suite yet.
- Local machine deps live at `~/linux`, `~/qemu`, `~/util-linux`, `~/ksmbd-tools`, `~/emsdk`, `~/wasm-sysroot`, `~/linux-wasm` (repo `deps/` is not synced locally); CI uses `deps/*` via peru.
- `xdqi/llvm-wasm` exists (default branch `wasm-18.1.2`; peru pins `wasm-18.1.2-anyfs`) but has **no releases** and only upstream LLVM workflows; there is no local clone. The expected wasm-ld path convention is `<checkout>/workspace/install/llvm/bin/wasm-ld` and doctor's hint is `./linux-wasm.sh build-llvm` — inspect the `wasm-18.1.2-anyfs` branch layout before writing the release workflow (Task 9A Step 1).
- wasm-sysroot provenance: only libblkid has an in-repo recipe (`build_libblkid_wasm.sh`); glib/gio/gobject/gmodule/gthread, pcre2 (×4), libffi, zlib, libzstd, libbz2, libuuid, libresolv were hand-built and are undocumented. Authoritative version pins live in `~/wasm-sysroot/lib/pkgconfig/*.pc`.
- Dependency provisioning today: linux-amd64 = apt system packages; mingw32/64 = prebuilt pacman packages fetched from `msys.kosaka.moe/repo` (bootstrap.tar.xz + `pacman -S`); wasm = the undocumented hand-built sysroot. Phase 2B moves wasm to the mingw model (prebuilt artifact + fetch).
- Playwright E2E (`ts/tests/e2e`) already runs 3 projects — `web`, `electron-native`, `electron-wasm` — over shared flows (open/browse/download, url-load incl. local Range server, formats matrix, errors) + `electron-only/backend-switch`. The legacy CDP suite (`tests/test-cdp.mjs` + `run-all.mjs`, 6 target×source combos) therefore overlaps heavily; parity must be checked combo-by-combo before demoting it (Task 18A).

---

## Phase 1 — finish config.sh migration

### Task 1: New config keys + machine-local override file

**Files:**
- Modify: `build.config.toml`
- Modify: `scripts/lib/config.sh`
- Modify: `build.user.toml.example`
- Create: `build.user.toml` (gitignored — local machine only)

- [ ] **Step 1: Add `wasm_sysroot` to `[paths]` in `build.config.toml`**

After the `ksmbd_tools` line add:

```toml
# Sysroot holding wasm static libs (libblkid/libz/libbz2/libzstd/glib...)
# produced by build_libblkid_wasm.sh and friends. "" => <repo>/wasm-sysroot
wasm_sysroot = ""
```

- [ ] **Step 2: Materialize the new defaults in `scripts/lib/config.sh`**

In `anyfs_load_config()`, after the existing `: "${ANYFS_PATHS_KSMBD_TOOLS:=...}"` line, add:

```bash
    : "${ANYFS_PATHS_WASM_SYSROOT:=$root/wasm-sysroot}"
    : "${ANYFS_TOOLCHAINS_WASM_LD:=$pfx$deps/llvm-wasm/workspace/install/llvm/bin/wasm-ld}"
```

and extend the final `export` line to include `ANYFS_PATHS_WASM_SYSROOT ANYFS_TOOLCHAINS_WASM_LD`. (The `wasm_ld` key already exists in the toml; only the `""`→default materialization is missing. The default path matches what `scripts/doctor.sh:36` already probes.)

- [ ] **Step 3: Document the new key in `build.user.toml.example`**

Add a commented sample:

```toml
# [paths]
# wasm_sysroot = "/path/to/wasm-sysroot"
```

- [ ] **Step 4: Write the local `build.user.toml`** (preserves current local builds once script defaults move from `$HOME` to `deps/`):

```toml
# Machine-local overrides (gitignored). This host keeps source trees in $HOME
# instead of peru-synced deps/.
[paths]
linux_src    = "/home/kosaka/linux"
qemu_src     = "/home/kosaka/qemu"
util_linux   = "/home/kosaka/util-linux"
ksmbd_tools  = "/home/kosaka/ksmbd-tools"
wasm_sysroot = "/home/kosaka/wasm-sysroot"

[toolchains]
emsdk   = "/home/kosaka/emsdk"
wasm_ld = "/home/kosaka/linux-wasm/workspace/install/llvm/bin/wasm-ld"
```

- [ ] **Step 5: Verify**

Run: `bash -c 'source scripts/lib/config.sh && echo "$ANYFS_PATHS_WASM_SYSROOT"; echo "$ANYFS_TOOLCHAINS_WASM_LD"; echo "$ANYFS_PATHS_QEMU_SRC"'`
Expected: `/home/kosaka/wasm-sysroot`, `/home/kosaka/linux-wasm/.../wasm-ld`, `/home/kosaka/qemu` (user.toml wins).
Also run with the override file moved away (`mv build.user.toml /tmp && bash -c '...' && mv /tmp/build.user.toml .`) — expected: `<repo>/wasm-sysroot`, `<repo>/deps/llvm-wasm/...`, `<repo>/deps/qemu`.

- [ ] **Step 6: Commit**

```bash
git add build.config.toml scripts/lib/config.sh build.user.toml.example
git commit -m "feat(build): add wasm_sysroot config key + materialize wasm_ld default"
```

### Task 2: Migrate `gen_lkl_config_wasm.sh` + `build_lkl_wasm.sh`

**Files:**
- Modify: `scripts/gen_lkl_config_wasm.sh:27-29`
- Modify: `scripts/build_lkl_wasm.sh:24-26,92-96`
- Modify: `scripts/lint-no-hardcoded-paths.sh:6`

- [ ] **Step 1 (red): Add both scripts to the lint allowlist**

In `scripts/lint-no-hardcoded-paths.sh` change line 6 to:

```bash
migrated='scripts/gen_lkl_config.sh scripts/build_lkl.sh scripts/gen_lkl_config_wasm.sh scripts/build_lkl_wasm.sh'
```

Run: `./scripts/lint-no-hardcoded-paths.sh` — Expected: FAIL listing the `$HOME` lines in both scripts.

- [ ] **Step 2: Migrate `gen_lkl_config_wasm.sh`**

Replace lines 27–29:

```bash
LINUX_DIR="$HOME/linux"
OUT_PARENT="$HOME/anyfs-reader"
EMSDK_DIR="$HOME/emsdk"
```

with (mirroring the already-migrated `gen_lkl_config.sh`):

```bash
# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"

# CLI --linux=/--out=/--emsdk= win; config.sh provides the defaults.
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT_PARENT="${OUT_PARENT:-$(cd "$(dirname "$0")/.." && pwd)}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"
```

Also update the header comment's option defaults (`default: ~/linux` → `default: from build.config.toml; linux_src or deps/linux`, etc.) so no `$HOME` text remains.

- [ ] **Step 3: Migrate `build_lkl_wasm.sh`**

Same replacement for its lines 24–26. Then replace line 92:

```bash
JOEL_WASM_LD="$HOME/linux-wasm/workspace/install/llvm/bin/wasm-ld"
```

with:

```bash
JOEL_WASM_LD="${JOEL_WASM_LD:-$ANYFS_TOOLCHAINS_WASM_LD}"
```

and update the not-found error message to say `set toolchains.wasm_ld in build.user.toml or build it: (cd deps/llvm-wasm && ./linux-wasm.sh build-llvm)`. Reword any remaining `$HOME` mentions in the header comment.

- [ ] **Step 4 (green): Verify**

Run: `./scripts/lint-no-hardcoded-paths.sh` — Expected: PASS.
Run: `bash -n scripts/gen_lkl_config_wasm.sh scripts/build_lkl_wasm.sh` — Expected: no output.
Run: `./scripts/build_lkl_wasm.sh --help` — Expected: usage text, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/gen_lkl_config_wasm.sh scripts/build_lkl_wasm.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "refactor(build): wire gen_lkl_config_wasm + build_lkl_wasm to build.config.toml"
```

### Task 3: Migrate `build_boot_wasm.sh` + `build_libblkid_wasm.sh`

**Files:**
- Modify: `scripts/build_boot_wasm.sh:27-29`
- Modify: `scripts/build_libblkid_wasm.sh:31-32,36-41`
- Modify: `scripts/lint-no-hardcoded-paths.sh:6` (append both)

- [ ] **Step 1 (red): Append both scripts to the lint allowlist; run lint; expect FAIL.**

- [ ] **Step 2: Migrate `build_boot_wasm.sh`** — replace lines 27–29 with:

```bash
# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT="${OUT:-$REPO_ROOT/lkl-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"
```

- [ ] **Step 3: Migrate `build_libblkid_wasm.sh`** — replace lines 31–32 with:

```bash
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

UL_SRC="${UL_SRC:-$ANYFS_PATHS_UTIL_LINUX}"
SYSROOT="${SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
```

(`SCRIPT_DIR`/`REPO_ROOT` already exist in this script.) Then make the emsdk activation fall back to config when `EMSDK_ENV` is unset — replace the `if [[ -n "${EMSDK_ENV:-}" ...` block's precondition with:

```bash
EMSDK_ENV="${EMSDK_ENV:-${ANYFS_TOOLCHAINS_EMSDK:+$ANYFS_TOOLCHAINS_EMSDK/emsdk_env.sh}}"
if [[ -n "${EMSDK_ENV:-}" && -f "$EMSDK_ENV" ]]; then
```

Reword the header comments and the `emconfigure not on PATH` error so they reference `toolchains.emsdk` in build.config.toml instead of `$HOME/emsdk` (the lint matches `\$HOME` even in strings).

- [ ] **Step 4 (green):** lint PASS; `bash -n` both; `./scripts/build_libblkid_wasm.sh --help 2>/dev/null || true` (script has no --help; instead run with `UL_SRC=/nonexistent` and expect its own clear error, not a `$HOME` path).

- [ ] **Step 5: Commit**

```bash
git add scripts/build_boot_wasm.sh scripts/build_libblkid_wasm.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "refactor(build): wire build_boot_wasm + build_libblkid_wasm to build.config.toml"
```

### Task 4: Migrate `build_libblkid_mingw.sh`

**Files:**
- Modify: `scripts/build_libblkid_mingw.sh:34-40,48`
- Modify: `scripts/lint-no-hardcoded-paths.sh:6` (append)

- [ ] **Step 1 (red):** append to allowlist, lint FAIL.
- [ ] **Step 2:** The target-selection `case` (lines 31–43) consumes `CROSS_PREFIX` defaults *before* `SCRIPT_DIR` is defined, so add the config source right after `set -euo pipefail` using the dirname form: `source "$(dirname "$0")/lib/config.sh"`. Then change the two defaults:

```bash
    mingw64)
        CROSS_PREFIX="${CROSS_PREFIX:-$ANYFS_TOOLCHAINS_MSYS2_CROSS/bin/x86_64-w64-mingw32}"
        ;;
    mingw32)
        CROSS_PREFIX="${CROSS_PREFIX:-$ANYFS_TOOLCHAINS_MSYS2_CROSS/bin/i686-w64-mingw32}"
        ;;
```

and line 48: `UL_SRC="${UL_SRC:-$ANYFS_PATHS_UTIL_LINUX}"`. Reword the header comment's `$HOME/util-linux` mention.

- [ ] **Step 3 (green):** lint PASS; `bash -n`; run `./scripts/build_libblkid_mingw.sh badtarget` → expected usage error exit 1.
- [ ] **Step 4: Commit** — `git commit -m "refactor(build): wire build_libblkid_mingw to build.config.toml"`

### Task 5: Migrate `build_qemu.sh`

**Files:**
- Modify: `scripts/build_qemu.sh:33,118-129` (+ header comments)
- Modify: `scripts/lint-no-hardcoded-paths.sh:6` (append)

- [ ] **Step 1 (red):** append to allowlist, lint FAIL.
- [ ] **Step 2:** Below `SCRIPT_DIR=` add `source "$SCRIPT_DIR/lib/config.sh"`, change line 33 to `QEMU_SRC="${QEMU_SRC:-$ANYFS_PATHS_QEMU_SRC}"`, and in `configure_for()` replace the four `/opt/msys2-cross/...` literals:

```bash
                "--extra-cflags=-I$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw32/include" \
                "--extra-ldflags=-L$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw32/lib"
```

(and the mingw64 pair likewise). Reword header comments mentioning `~/qemu` and `/opt/msys2-cross`.
- [ ] **Step 3 (green):** lint PASS; `bash -n`; `./scripts/build_qemu.sh --help` → usage, exit 0.
- [ ] **Step 4: Commit** — `git commit -m "refactor(build): wire build_qemu to build.config.toml"`

### Task 6: Migrate `build_anyfs.sh`

**Files:**
- Modify: `scripts/build_anyfs.sh:44-46,374-377` (+ header comments)
- Modify: `scripts/lint-no-hardcoded-paths.sh:6` (append)

- [ ] **Step 1 (red):** append to allowlist, lint FAIL.
- [ ] **Step 2:** Below `SRC_DIR=` add:

```bash
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

# Meson refuses absolute paths that point inside the source tree (see NOTE
# below), so config defaults are re-relativized against SRC_DIR when inside it.
rel_to_src() {
    case "$1" in
        "$SRC_DIR"/*) printf '%s\n' "${1#"$SRC_DIR"/}" ;;
        *)            printf '%s\n' "$1" ;;
    esac
}
```

then replace lines 44–46:

```bash
QEMU_ROOT="$(rel_to_src "$ANYFS_PATHS_QEMU_SRC")"
KSMBD_ROOT="$(rel_to_src "$ANYFS_PATHS_KSMBD_TOOLS")"
LKL_SRC="$(rel_to_src "$ANYFS_PATHS_LINUX_SRC")"
```

(CLI `--qemu-root/--ksmbd-root/--lkl-src` still override afterwards — they're parsed later, unchanged.) Replace `mingw_sysroot_for()`'s two literals with `echo "$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw32"` / `.../mingw64`. Reword header-comment defaults (`default: ~/qemu` → `default: from build.config.toml`).
- [ ] **Step 3 (green):** lint PASS; `bash -n`; `./scripts/build_anyfs.sh --help` → usage, exit 0.
- [ ] **Step 4: Full local verification of Phase 1** — rebuild one cheap target end-to-end to prove config resolution works:

Run: `./scripts/build_anyfs.sh --targets=linux-amd64 --components=core,server,fuse -j"$(nproc)"`
Expected: completes, `build-anyfs-linux-amd64/bin/` populated (this uses the local `build.user.toml` paths).
- [ ] **Step 5: Commit** — `git commit -m "refactor(build): wire build_anyfs to build.config.toml; lint covers all native scripts"`

### Task 6A: shellcheck gate

**Files:**
- Create: `scripts/lint-shellcheck.sh`
- Modify: `.github/workflows/linux.yml` (lint step)

- [ ] **Step 1:** Write `scripts/lint-shellcheck.sh` — same allowlist philosophy as the hardcoded-path lint (scripts get added as they're cleaned, starting with everything migrated in this plan):

```bash
#!/usr/bin/env bash
# shellcheck gate for build scripts (severity >= warning). Extend the list
# as scripts are cleaned; new scripts must be added here.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
checked='
scripts/lib/config.sh
scripts/lib/wasm_exports.sh
scripts/gen_lkl_config.sh
scripts/build_lkl.sh
scripts/gen_lkl_config_wasm.sh
scripts/build_lkl_wasm.sh
scripts/build_boot_wasm.sh
scripts/build_libblkid_wasm.sh
scripts/build_libblkid_mingw.sh
scripts/build_qemu.sh
scripts/build_anyfs.sh
scripts/build_anyfs_wasm.sh
scripts/lint-no-hardcoded-paths.sh
scripts/lint-shellcheck.sh
'
# shellcheck disable=SC2086
shellcheck -x -S warning $(printf "$root/%s " $checked)
```

`chmod +x scripts/lint-shellcheck.sh`. Fix every finding it reports in the listed scripts (or annotate with `# shellcheck disable=SCxxxx` + a reason where the pattern is intentional).
- [ ] **Step 2:** In `.github/workflows/linux.yml`, extend the lint step:

```yaml
      - name: Lint — no hardcoded paths in migrated build scripts
        run: ./scripts/lint-no-hardcoded-paths.sh

      - name: Lint — shellcheck
        run: |
          sudo apt-get install -y --no-install-recommends shellcheck
          ./scripts/lint-shellcheck.sh
```

- [ ] **Step 3: Verify** — `./scripts/lint-shellcheck.sh` exits 0 locally.
- [ ] **Step 4: Commit** — `git commit -m "ci(lint): shellcheck gate for build scripts"`

(`scripts/lib/wasm_exports.sh` and `scripts/build_anyfs_wasm.sh` enter this list in Phase 2 — if executing strictly in order, add them to `checked` during Task 8 instead of here.)

---

## Phase 2 — single-source wasm exports + unify on build_anyfs_wasm.sh

### Task 7: Export-list generator + drift gate (TDD)

**Files:**
- Create: `scripts/lib/wasm_exports.sh`
- Create: `tests/test_wasm_exports.sh`

- [ ] **Step 1: Write the failing test**

`tests/test_wasm_exports.sh`:

```bash
#!/usr/bin/env bash
# Gate for the generated wasm export list:
#   1. the generator emits every known-core symbol,
#   2. every ccall'd anyfs_ts_* name in the TS worker layer is exported
#      (catches TS<->C drift that previously bit build_anyfs_browser_wasm.sh).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../scripts/lib/wasm_exports.sh
source "$root/scripts/lib/wasm_exports.sh"

list="$(anyfs_wasm_exports "$root/ts/native/anyfs_ts.c")"

for must in _main _malloc _free _anyfs_ts_kernel_init _anyfs_ts_init_async \
            _anyfs_ts_session_open _anyfs_ts_session_enter_async \
            _anyfs_ts_session_enter_result_p _anyfs_ts_pread_p _anyfs_ts_close_p; do
    [[ ",$list," == *",$must,"* ]] || { echo "FAIL: $must missing from generated exports"; exit 1; }
done

n="$(tr ',' '\n' <<<"$list" | grep -c '^_anyfs_ts_')"
[[ "$n" -ge 30 ]] || { echo "FAIL: only $n anyfs_ts_* exports (expected >= 30)"; exit 1; }

# TS drift gate: every ccall('anyfs_ts_...') in the worker layer must be exported.
missing=0
while IFS= read -r sym; do
    [[ ",$list," == *",_$sym,"* ]] || { echo "FAIL: worker ccalls $sym but it is not exported"; missing=1; }
done < <(grep -rhoE "ccall\(\s*'(anyfs_ts_[a-z0-9_]+)'" \
            "$root/ts/packages/core/src" | grep -oE 'anyfs_ts_[a-z0-9_]+' | sort -u)
[[ "$missing" -eq 0 ]]

echo "OK: $n anyfs_ts_* exports, worker ccalls all covered"
```

`chmod +x tests/test_wasm_exports.sh`

- [ ] **Step 2: Run to verify it fails**

Run: `./tests/test_wasm_exports.sh`
Expected: FAIL — `scripts/lib/wasm_exports.sh: No such file or directory`.

- [ ] **Step 3: Write the generator**

`scripts/lib/wasm_exports.sh`:

```bash
# scripts/lib/wasm_exports.sh — derive -sEXPORTED_FUNCTIONS for the wasm
# bundle from the TS glue source. ts/native/anyfs_ts.c is the single source
# of truth: every non-static anyfs_ts_* function defined at column 0 is
# exported (renaming a glue function can therefore never silently drop it
# from the bundle — the failure surfaces in the node smoke test instead).
anyfs_wasm_exports() {
    local glue="$1" syms s out
    syms="$(grep -hE '^[A-Za-z_][A-Za-z0-9_* ]*[ *]anyfs_ts_[A-Za-z0-9_]+\(' "$glue" \
              | grep -v '^static' \
              | grep -oE 'anyfs_ts_[A-Za-z0-9_]+' \
              | sort -u)"
    if [[ -z "$syms" ]]; then
        echo "wasm_exports: no anyfs_ts_* definitions found in $glue" >&2
        return 1
    fi
    out="_main,_malloc,_free"
    for s in $syms; do out+=",_$s"; done
    printf '%s\n' "$out"
}
```

- [ ] **Step 4: Run the test — expect PASS.** If the symbol count differs from the hand-written list, diff them before proceeding:

```bash
bash -c 'source scripts/lib/wasm_exports.sh; anyfs_wasm_exports ts/native/anyfs_ts.c | tr "," "\n" | sort' > /tmp/gen.txt
grep -A14 "^EXPORTED_FUNCS=" scripts/build_anyfs_wasm.sh | tr -d "'\\\\" | tr ',' '\n' | grep '^_' | sort > /tmp/hand.txt
diff /tmp/hand.txt /tmp/gen.txt
```

Expected: empty diff (the hand list has 38 entries: `_main,_malloc,_free` + 35 glue symbols). Any extra symbol in `gen.txt` means anyfs_ts.c gained a function the hand list missed — that is the bug class this fixes; keep the generated version.

- [ ] **Step 5: Commit**

```bash
git add scripts/lib/wasm_exports.sh tests/test_wasm_exports.sh
git commit -m "feat(build): generate wasm export list from anyfs_ts.c + drift gate"
```

### Task 8: build_anyfs_wasm.sh consumes the generator + config.sh; delete the stale browser script

**Files:**
- Modify: `scripts/build_anyfs_wasm.sh:28-39,63-68,164-165,188-205`
- Delete: `scripts/build_anyfs_browser_wasm.sh` (untracked — plain `rm`)
- Modify: `scripts/lint-no-hardcoded-paths.sh:6` (append `scripts/build_anyfs_wasm.sh`)

- [ ] **Step 1 (red):** append `scripts/build_anyfs_wasm.sh` to the lint allowlist; lint FAIL.
- [ ] **Step 2: Migrate the header block.** Replace lines 28–39 with:

```bash
# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"
# shellcheck source=lib/wasm_exports.sh
source "$(dirname "$0")/lib/wasm_exports.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT="${OUT:-$REPO_ROOT/lkl-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"
BLD="${BLD:-$REPO_ROOT/build-anyfs-wasm}"
TS="${TS:-$REPO_ROOT/ts}"
QEMU_ROOT="${QEMU_ROOT:-$ANYFS_PATHS_QEMU_SRC}"
QBLD="${QBLD:-$QEMU_ROOT/build-anyfs-wasm}"
SYS="${WASM_SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
SRC_CORE="${SRC_CORE:-$REPO_ROOT/src/core}"
GLUE="$TS/native/anyfs_ts.c"
LIBLKL="$OUT/tools/lkl/liblkl.a"
TARGET="${ANYFS_TARGET:-browser}"
```

- [ ] **Step 3:** Replace the two remaining `-I "$HOME/anyfs-reader/include"` occurrences (in the `INC` array ~line 66 and the qemu_backend.c compile ~line 164) with `-I "$REPO_ROOT/include"`. Reword header comments mentioning `$HOME`.
- [ ] **Step 4:** Replace the 14-line `EXPORTED_FUNCS='...'` literal (lines 192–205) with:

```bash
EXPORTED_FUNCS="$(anyfs_wasm_exports "$GLUE")"
```

- [ ] **Step 5:** `rm scripts/build_anyfs_browser_wasm.sh`, then confirm nothing live references it:

Run: `grep -rn "build_anyfs_browser_wasm" --include="*.sh" --include="*.yml" --include="*.json" --include="*.ts" --include="*.mjs" . | grep -v node_modules | grep -v docs/`
Expected: no output (the only mention is the historical design doc, which stays).

- [ ] **Step 6 (green):** lint PASS; `bash -n scripts/build_anyfs_wasm.sh`; `./tests/test_wasm_exports.sh` PASS.
- [ ] **Step 7: Commit**

```bash
git add scripts/build_anyfs_wasm.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "refactor(build): build_anyfs_wasm uses config.sh + generated exports; drop stale browser_wasm script"
```

### Task 9: Prove the bundle still works (full wasm rebuild + smoke)

**Files:** none (build + test only)

- [ ] **Step 1:** Rebuild the node-target bundle: `ANYFS_TARGET=node ./scripts/build_anyfs_wasm.sh`
Expected: links `ts/packages/core/wasm/anyfs.node.mjs` without `undefined symbol` errors.
- [ ] **Step 2:** Rebuild the browser bundle: `./scripts/build_anyfs_wasm.sh`
Expected: `ts/packages/core/wasm/anyfs.mjs` regenerated.
- [ ] **Step 3:** Run the existing core smoke suite (exercises ccall→export coverage end-to-end):

```bash
cd ts && pnpm --filter @anyfs/core build && pnpm --filter @anyfs/core test
```

Expected: `smoke.node.mjs single|multi|big` all PASS.
- [ ] **Step 4: Commit** (wasm artifacts if they are tracked; check `git status -- ts/packages/core/wasm` first — if untracked, no commit needed):

```bash
git add -A ts/packages/core/wasm 2>/dev/null || true
git commit -m "build(wasm): regenerate bundles from unified export list" || echo "nothing tracked to commit"
```

---

## Phase 2A — wasm-ld release pipeline (xdqi/llvm-wasm fork)

**Why:** the patched wasm-ld (GNU-ld SECTIONS{} support for the LKL vmlinux link) currently requires every machine to build LLVM from `~/linux-wasm` sources. The fork gets a CI release pipeline (accelerated by the sccache-dist farm — LLVM is a huge C++ build, the ideal customer); anyfs-reader consumes the release binary.

### Task 9A: Release workflow in the llvm-wasm fork

**Files (in a fresh clone, NOT in anyfs-reader):**
- Clone: `gh repo clone xdqi/llvm-wasm ~/llvm-wasm -- --branch wasm-18.1.2-anyfs --depth 1`
- Create: `~/llvm-wasm/.github/workflows/wasm-ld-release.yml`

- [ ] **Step 1: Inspect the branch layout** — `ls ~/llvm-wasm` and check whether it is (a) a plain llvm-project fork (has `llvm/`, `lld/` at root) or (b) carries Joel's `linux-wasm.sh` harness (doctor's hint suggests the consumed artifact lives at `workspace/install/llvm/bin/wasm-ld`). Record which; the build step below assumes (a) — if (b), replace the cmake/ninja block with `CC="sccache clang" CXX="sccache clang++" ./linux-wasm.sh build-llvm` and adjust the artifact path to `workspace/install/llvm/bin/wasm-ld`.
- [ ] **Step 2: Write `.github/workflows/wasm-ld-release.yml`**

```yaml
name: wasm-ld-release

# Builds the patched wasm-ld (GNU-ld linker-script support for LKL's
# vmlinux link) and publishes it as a GitHub release asset consumed by
# anyfs-reader's scripts/fetch_wasm_ld.sh. Compilation is distributed
# over an ephemeral sccache-dist farm.

on:
  workflow_dispatch:
    inputs:
      tag:
        description: 'release tag (e.g. wasm-ld-18.1.2-anyfs-r1)'
        required: true

jobs:
  sccache-workers:
    name: sccache worker ${{ matrix.idx }}
    runs-on: ubuntu-24.04
    timeout-minutes: 120
    strategy:
      matrix:
        idx: [1, 2, 3]
    steps:
      - uses: xdqi/sccache-dist-action@v0.0.4
        with:
          mode: worker
          worker-index: '${{ matrix.idx }}'
          oauth-secret: '${{ secrets.TS_OAUTH_SECRET }}'

  build:
    runs-on: ubuntu-24.04
    timeout-minutes: 120
    permissions:
      contents: write
    steps:
      - uses: actions/checkout@v4
      - name: Install deps
        run: sudo apt-get update && sudo apt-get install -y --no-install-recommends cmake ninja-build clang lld python3
      - uses: actions/cache@v4
        with:
          path: ~/.cache/sccache
          key: sccache-wasm-ld-${{ github.sha }}
          restore-keys: |
            sccache-wasm-ld-
      - name: Bring up sccache-dist farm
        uses: xdqi/sccache-dist-action@v0.0.4
        with:
          mode: coordinator
          expected-workers: 3
          min-workers: 1
          oauth-secret: '${{ secrets.TS_OAUTH_SECRET }}'
      - name: Build wasm-ld
        run: |
          cmake -S llvm -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_C_COMPILER_LAUNCHER=sccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
            -DLLVM_ENABLE_PROJECTS=lld \
            -DLLVM_TARGETS_TO_BUILD=WebAssembly \
            -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
          ninja -C build -j"${SCCACHE_J:-$(nproc)}" lld
          sccache --show-stats
      - name: Package
        run: |
          mkdir -p out
          cp build/bin/wasm-ld out/wasm-ld   # lld symlink target; verify with `build/bin/wasm-ld --version`
          strip out/wasm-ld
          out/wasm-ld --version | grep -q 'LLD 18'
          tar -C out -cJf wasm-ld-linux-amd64.tar.xz wasm-ld
      - name: Release
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          gh release create "${{ inputs.tag }}" wasm-ld-linux-amd64.tar.xz \
            --target "$GITHUB_SHA" \
            --title "${{ inputs.tag }}" \
            --notes "Patched wasm-ld (GNU-ld SECTIONS{} support) for anyfs-reader's LKL wasm link."
```

**Note:** `bin/wasm-ld` is a symlink to `lld` in LLVM builds — `cp` dereferences it, which is what we want. If the patch set lives in `lld/wasm/` of this fork, the plain cmake build picks it up automatically.
- [ ] **Step 3:** Verify the fork has the `TS_OAUTH_SECRET` secret (`gh secret list -R xdqi/llvm-wasm`); set it if absent (same Tailscale OAuth client as anyfs-reader's).
- [ ] **Step 4:** Commit to a branch of the fork, push, and trigger: `gh workflow run wasm-ld-release.yml -R xdqi/llvm-wasm --ref <branch> -f tag=wasm-ld-18.1.2-anyfs-r1`. Watch with `gh run watch -R xdqi/llvm-wasm`. Expected: release `wasm-ld-18.1.2-anyfs-r1` exists with the tarball; sccache stats show distributed compiles.
- [ ] **Step 5:** Smoke the artifact locally: download, untar, `./wasm-ld --version` → `LLD 18...`; then run one real LKL wasm link with `JOEL_WASM_LD=<downloaded> ./scripts/build_lkl_wasm.sh` → links clean.

### Task 9B: Consume the release in anyfs-reader

**Files:**
- Create: `scripts/fetch_wasm_ld.sh`
- Modify: `scripts/lib/config.sh` (default chain)
- Modify: `scripts/doctor.sh` (accept the fetched location + suggest the fetch script)
- Modify: `scripts/lint-shellcheck.sh` + `scripts/lint-no-hardcoded-paths.sh` (add fetch_wasm_ld.sh)

- [ ] **Step 1:** Write `scripts/fetch_wasm_ld.sh`:

```bash
#!/usr/bin/env bash
# Download the patched wasm-ld release (built by xdqi/llvm-wasm's
# wasm-ld-release workflow) into <repo>/.toolchain/wasm-ld/.
# Pin: bump WASM_LD_TAG when a new release is cut.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
WASM_LD_TAG="${WASM_LD_TAG:-wasm-ld-18.1.2-anyfs-r1}"
dest="$root/.toolchain/wasm-ld"
if [[ -x "$dest/wasm-ld" ]] && "$dest/wasm-ld" --version | grep -q 'LLD 18'; then
    echo "wasm-ld already present: $dest/wasm-ld"; exit 0
fi
mkdir -p "$dest"
url="https://github.com/xdqi/llvm-wasm/releases/download/$WASM_LD_TAG/wasm-ld-linux-amd64.tar.xz"
echo "fetching $url"
curl -fsSL --retry 3 "$url" | tar -xJ -C "$dest"
"$dest/wasm-ld" --version | grep -q 'LLD 18'
echo "OK: $dest/wasm-ld"
```

`chmod +x`, add `.toolchain/` to `.gitignore`.
- [ ] **Step 2:** In `scripts/lib/config.sh`, change the wasm_ld default chain to prefer the fetched binary when present:

```bash
    if [[ -z "${ANYFS_TOOLCHAINS_WASM_LD:-}" && -x "$root/.toolchain/wasm-ld/wasm-ld" ]]; then
        ANYFS_TOOLCHAINS_WASM_LD="$root/.toolchain/wasm-ld/wasm-ld"
    fi
    : "${ANYFS_TOOLCHAINS_WASM_LD:=$pfx$deps/llvm-wasm/workspace/install/llvm/bin/wasm-ld}"
```

(Explicit `build.user.toml` still wins because it arrives non-empty.)
- [ ] **Step 3:** Update `scripts/doctor.sh`'s wasm-ld failure hint to: `run scripts/fetch_wasm_ld.sh (or build from deps/llvm-wasm)`.
- [ ] **Step 4: Verify** — `rm -rf .toolchain && ./scripts/fetch_wasm_ld.sh` downloads + validates; `bash -c 'source scripts/lib/config.sh && echo $ANYFS_TOOLCHAINS_WASM_LD'` → the `.toolchain` path when `build.user.toml`'s `wasm_ld` is commented out; shellcheck + both lint gates PASS.
- [ ] **Step 5: Commit** — `git commit -m "feat(build): fetch patched wasm-ld from llvm-wasm fork releases"`

---

## Phase 2B — wasm-sysroot: recipe + prebuilt tarball (mingw provisioning model)

**Why:** today only libblkid has an in-repo recipe; the other ~19 static libs in `~/wasm-sysroot` were hand-built and are unreproducible. Target model = mingw's: an in-repo recipe (the PKGBUILD role), a CI-published tarball (the bootstrap.tar.xz role), a fetch script + doctor manifest check (the pacman role).

### Task 9C: Sysroot manifest + doctor check (do this first — it locks the contract)

**Files:**
- Create: `scripts/lib/wasm_sysroot.manifest`
- Modify: `scripts/doctor.sh`

- [ ] **Step 1:** Generate the manifest from the known-good local sysroot — record both the lib list and the versions that pkgconfig knows:

```bash
{ ls ~/wasm-sysroot/lib/*.a | xargs -n1 basename;
  for p in ~/wasm-sysroot/lib/pkgconfig/*.pc; do
      printf '# %s %s\n' "$(basename "$p" .pc)" "$(grep -m1 '^Version:' "$p" | cut -d' ' -f2)";
  done; } | sort > scripts/lib/wasm_sysroot.manifest
```

Review the output (expect ~20 `.a` entries: libblkid, libbz2, libffi, libgio-2.0, libglib-2.0, libgmodule-2.0, libgobject-2.0, libgthread-2.0, libgirepository-2.0, libpcre2-{8,16,32,posix}, libresolv, libuuid, libz, libzstd + version comment lines).
- [ ] **Step 2:** Add a doctor section that checks every `.a` in the manifest exists under `$ANYFS_PATHS_WASM_SYSROOT/lib` and reports missing ones with the hint `run scripts/fetch_wasm_sysroot.sh (or scripts/build_wasm_sysroot.sh)`.
- [ ] **Step 3: Verify** — `./scripts/doctor.sh` reports the sysroot section OK locally (and FAIL when pointed at an empty dir via `ANYFS_PATHS_WASM_SYSROOT=/tmp/empty ./scripts/doctor.sh`).
- [ ] **Step 4: Commit** — `git commit -m "feat(doctor): wasm-sysroot manifest check"`

### Task 9D: `build_wasm_sysroot.sh` recipe (exploratory — acceptance is manifest parity)

**Files:**
- Create: `scripts/build_wasm_sysroot.sh`
- Create: `scripts/lib/emscripten-cross.meson` (glib needs a meson cross file)
- Modify: lint allowlists (both)

This task is the archaeology: the recipe must rebuild, from pinned upstream tarballs, a sysroot whose `.a` set matches `scripts/lib/wasm_sysroot.manifest`. Versions come from the manifest's `#` comment lines (zlib/zstd/bz2 have no `.pc` — pin to the versions currently linkable; start with zlib 1.3.x, bzip2 1.0.8, zstd 1.5.x and verify the existing `.a`s' object names match).

- [ ] **Step 1:** Script skeleton — config-sourced, per-lib functions, `SYSROOT` output dir argument (default `$ANYFS_PATHS_WASM_SYSROOT`), `--only=<lib>` for iteration:

```bash
#!/usr/bin/env bash
# Build the complete wasm sysroot from pinned sources (the PKGBUILD role
# in the mingw-style provisioning model; see docs/wasm-sysroot.md).
# Order matters: zlib -> bz2 -> zstd -> libffi -> pcre2 -> glib -> util-linux(blkid+uuid).
set -euo pipefail
source "$(dirname "$0")/lib/config.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
WORK="${WORK:-$REPO_ROOT/build-wasm-sysroot}"
# ... per-lib build_zlib / build_bz2 / build_zstd / build_ffi / build_pcre2 /
#     build_glib / build_blkid functions; each: fetch pinned tarball into
#     $WORK, emconfigure/emcmake/meson-cross build, install into $SYSROOT.
```

Known-good per-lib approaches (verify/adjust while iterating):
  - zlib: `emconfigure ./configure --static --prefix=$SYSROOT && emmake make install`
  - bzip2: no configure — `emmake make libbz2.a CC=emcc AR=emar RANLIB=emranlib` + manual install
  - zstd: `emmake make -C lib libzstd.a` + manual install (or `emcmake cmake build/cmake -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF`)
  - libffi: needs the emscripten port patches — use upstream libffi + `emconfigure ./configure --host=wasm32-unknown-linux --disable-shared`; if that fails, the known-working route is the `libffi-emscripten` fork. Record which in the script comment.
  - pcre2: `emcmake cmake -DPCRE2_BUILD_PCRE2_16=ON -DPCRE2_BUILD_PCRE2_32=ON -DBUILD_SHARED_LIBS=OFF`
  - glib: `meson setup --cross-file scripts/lib/emscripten-cross.meson -Ddefault_library=static -Dtests=false -Dglib_assert=false`; the cross file maps c='emcc', ar='emar', pkg-config sysroot to `$SYSROOT`. glib-on-emscripten is the fiddly one — budget iteration time; the existing `~/wasm-sysroot` proves a working flag set exists.
  - blkid+uuid: reuse the logic from `build_libblkid_wasm.sh` (fold it in or call it; keep `build_libblkid_wasm.sh` as a thin wrapper for compat).
- [ ] **Step 2 (acceptance):** Build into a fresh dir and diff against the manifest:

```bash
SYSROOT=/tmp/sysroot-rebuild ./scripts/build_wasm_sysroot.sh
ls /tmp/sysroot-rebuild/lib/*.a | xargs -n1 basename | sort > /tmp/rebuilt.txt
grep -v '^#' scripts/lib/wasm_sysroot.manifest | sort > /tmp/expected.txt
diff /tmp/expected.txt /tmp/rebuilt.txt
```

Expected: empty diff (libgirepository may be droppable if nothing links it — check `grep -rn girepository scripts/ meson.build`; if unused, remove it from the manifest instead of building it, and note that in the commit message).
- [ ] **Step 3 (proof):** Rebuild the wasm bundle against the fresh sysroot and run the smoke suite: `WASM_SYSROOT=/tmp/sysroot-rebuild ANYFS_TARGET=node ./scripts/build_anyfs_wasm.sh && (cd ts && pnpm --filter @anyfs/core test)` → PASS.
- [ ] **Step 4:** shellcheck + lint gates; commit — `git commit -m "feat(build): reproducible wasm-sysroot recipe (manifest-parity with the legacy hand-built sysroot)"`

### Task 9E: CI tarball release + fetch script

**Files:**
- Create: `.github/workflows/wasm-sysroot.yml`
- Create: `scripts/fetch_wasm_sysroot.sh`
- Create: `docs/wasm-sysroot.md`

- [ ] **Step 1:** `.github/workflows/wasm-sysroot.yml` — workflow_dispatch with a `tag` input (e.g. `wasm-sysroot-r1`): install emsdk (`mymindstorm/setup-emsdk@v14`, version matching local — check `emcc --version` locally and pin it), meson/ninja/cmake, run `SYSROOT="$PWD/out-sysroot" ./scripts/build_wasm_sysroot.sh`, run the manifest diff from Task 9D Step 2 as the gate, `tar -cJf wasm-sysroot-linux.tar.xz -C out-sysroot .`, `gh release create "$tag" ...` (same shape as Task 9A's release step, `permissions: contents: write`).
- [ ] **Step 2:** `scripts/fetch_wasm_sysroot.sh` — mirror of `fetch_wasm_ld.sh`: pin `WASM_SYSROOT_TAG`, download from `https://github.com/xdqi/anyfs/releases/download/$WASM_SYSROOT_TAG/wasm-sysroot-linux.tar.xz` into `<repo>/.toolchain/wasm-sysroot/`, presence check = manifest check, and the same config.sh preference hook:

```bash
    if [[ -z "${ANYFS_PATHS_WASM_SYSROOT:-}" && -d "$root/.toolchain/wasm-sysroot/lib" ]]; then
        ANYFS_PATHS_WASM_SYSROOT="$root/.toolchain/wasm-sysroot"
    fi
```

(insert before the existing `: "${ANYFS_PATHS_WASM_SYSROOT:=$root/wasm-sysroot}"`).
- [ ] **Step 3:** `docs/wasm-sysroot.md` — document the three-role model (recipe=PKGBUILD, release tarball=bootstrap.tar.xz, fetch+manifest=pacman), version-bump procedure, and the per-target provisioning table (apt / msys.kosaka.moe pacman / this tarball).
- [ ] **Step 4:** Trigger the workflow, verify the release exists, then on a clean checkout simulate a fresh machine: `mv ~/wasm-sysroot ~/wasm-sysroot.bak; ./scripts/fetch_wasm_sysroot.sh && ./scripts/doctor.sh` → sysroot section OK; restore `~/wasm-sysroot`.
- [ ] **Step 5: Commit** — `git commit -m "feat(build): wasm-sysroot CI tarball release + fetch (mingw-style provisioning)"`

---

## Phase 3 — extract server_common from ksmbd/nfsd

### Task 10: Create `src/server_common/` + meson wiring

**Files:**
- Create: `src/server_common/server_common.h`
- Create: `src/server_common/server_common.c`
- Modify: `meson.build` (the `anyfs-ksmbd` and `anyfs-nfsd` `executable()` blocks — grep `lkl_ksmbd = executable`)

- [ ] **Step 1: Write `src/server_common/server_common.h`**

```c
/*
 * server_common.h — Shared daemon skeleton for the LKL server surfaces
 * (anyfs-ksmbd, anyfs-nfsd): stop flag + signal install, kernel boot with
 * loopback up, the --share resolution loop, and shutdown. Arg parsing and
 * the serving loops stay in the respective mains.
 */
#ifndef ANYFS_SERVER_COMMON_H
#define ANYFS_SERVER_COMMON_H

#include "anyfs.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while serving; cleared by SIGINT/SIGTERM. Serving loops poll it. */
extern volatile sig_atomic_t anyfs_server_running;

/* Unbuffer stdout and route SIGINT/SIGTERM to clearing
 * anyfs_server_running. */
void anyfs_server_install_signals(void);

/* Boot the LKL kernel and bring up loopback (ifindex 1, idempotent —
 * the in-kernel listeners bind to it). Returns anyfs_kernel_init()'s
 * result: 0 on success. */
int anyfs_server_boot(const AnyfsKernelOpts* opts);

typedef struct AnyfsShareEntry {
	char name[64];
	char lkl_path[ANYFS_LKL_PATH_MAX];
} AnyfsShareEntry;

/* Resolve every --share spec to a (name, lkl_path) pair via
 * anyfs_share_resolve(), which prints its own diagnostics. Returns the
 * number of entries filled, or -1 on the first failure. */
int anyfs_server_resolve_shares(char* const* specs, int n_specs,
				AnyfsSession** disks, int n_disks,
				uint32_t enter_flags, AnyfsShareEntry* out,
				int max_out);

/* Close every non-NULL session and halt the kernel. */
void anyfs_server_shutdown(AnyfsSession** disks, int n_disks);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Write `src/server_common/server_common.c`**

```c
// SPDX-License-Identifier: GPL-2.0-or-later
#include "server_common.h"

#include <lkl.h>
#include <stdio.h>

volatile sig_atomic_t anyfs_server_running = 1;

static void handle_stop(int sig)
{
	(void)sig;
	anyfs_server_running = 0;
}

void anyfs_server_install_signals(void)
{
	setbuf(stdout, NULL);
	signal(SIGINT, handle_stop);
	signal(SIGTERM, handle_stop);
}

int anyfs_server_boot(const AnyfsKernelOpts* opts)
{
	int ret = anyfs_kernel_init(opts);
	if (ret)
		return ret;
	/* lo is auto-up after boot, but the call is idempotent and
	 * documents intent. */
	lkl_if_up(1);
	return 0;
}

int anyfs_server_resolve_shares(char* const* specs, int n_specs,
				AnyfsSession** disks, int n_disks,
				uint32_t enter_flags, AnyfsShareEntry* out,
				int max_out)
{
	int n = 0;

	for (int si = 0; si < n_specs; si++) {
		if (n >= max_out) {
			fprintf(stderr, "error: too many shares (max %d)\n",
				max_out);
			return -1;
		}
		AnyfsShareEntry* e = &out[n];
		if (anyfs_share_resolve(specs[si], disks, n_disks, enter_flags,
					e->name, sizeof(e->name), e->lkl_path,
					sizeof(e->lkl_path)) < 0)
			return -1;
		n++;
	}
	return n;
}

void anyfs_server_shutdown(AnyfsSession** disks, int n_disks)
{
	for (int i = 0; i < n_disks; i++) {
		if (disks[i])
			anyfs_session_close(disks[i]);
	}
	anyfs_kernel_halt();
}
```

(Verify `anyfs.h` transitively provides `AnyfsKernelOpts`, `ANYFS_LKL_PATH_MAX`, `anyfs_share_resolve`, `anyfs_session_close`, `anyfs_kernel_halt`, `anyfs_kernel_init` — both mains already get all of these from `"anyfs.h"` + `<lkl.h>`. If `lkl_if_up` needs another header, mirror whatever `ksmbd_main.c` includes.)

- [ ] **Step 3: Wire into meson.** In `meson.build`, add the source file and include dir to **both** server executables:
  - `lkl_ksmbd = executable('anyfs-ksmbd', ...)`: add `'src/server_common/server_common.c'` to the source list and `include_directories('src/server_common')` to its `include_directories:` array.
  - `lkl_nfsd = executable('anyfs-nfsd', ...)`: likewise.

- [ ] **Step 4: Compile check** (server_common is not yet referenced, it must just build):

Run: `./scripts/build_anyfs.sh --targets=linux-amd64 --components=core,server -j"$(nproc)"`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/server_common meson.build
git commit -m "feat(server): add server_common daemon skeleton (signals/boot/shares/shutdown)"
```

### Task 11: Port ksmbd_main.c to server_common

**Files:**
- Modify: `src/ksmbd/ksmbd_main.c`

- [ ] **Step 1:** Add `#include "server_common.h"` next to the other local includes.
- [ ] **Step 2:** Delete the local `static volatile int running = 1;` and `sigint_handler` (lines ~71–77); replace every remaining `running` reference (the `while (running)` IPC loop) with `anyfs_server_running`.
- [ ] **Step 3:** Replace the ShareInfo typedef with the shared entry — delete the `typedef struct { char name[64]; char lkl_path[ANYFS_LKL_PATH_MAX]; } ShareInfo;` block and add `typedef AnyfsShareEntry ShareInfo;` (keeps `setup_ksmbd_config(const ShareInfo*, ...)` and all `sh->name`/`sh->lkl_path` uses unchanged).
- [ ] **Step 4:** In `main()`:
  - replace the `setbuf(stdout, NULL); signal(SIGINT, sigint_handler); signal(SIGTERM, sigint_handler);` triple with `anyfs_server_install_signals();`
  - replace the kernel-boot block

    ```c
    int ret = anyfs_kernel_init(&kern_opts);
    if (ret) { pr_err("Failed to start kernel\n"); return 1; }
    pr_info("LKL kernel started (ksmbd built-in)\n");
    lkl_if_up(1);
    ```

    with

    ```c
    int ret = anyfs_server_boot(&kern_opts);
    if (ret) { pr_err("Failed to start kernel\n"); return 1; }
    pr_info("LKL kernel started (ksmbd built-in)\n");
    ```

    (delete the now-redundant standalone `lkl_if_up(1);` and its comment block — the comment content moved to server_common.c).
  - replace the share-resolve loop

    ```c
    ShareInfo shares[ANYFS_MAX_SHARES];
    int n_shares = 0;
    for (int si = 0; si < n_share_specs; si++) { ... }
    ```

    with

    ```c
    ShareInfo shares[ANYFS_MAX_SHARES];
    int n_shares = anyfs_server_resolve_shares(share_specs, n_share_specs,
                                               disks, n_images, 0, shares,
                                               ANYFS_MAX_SHARES);
    if (n_shares < 0)
        goto halt;
    for (int i = 0; i < n_shares; i++)
        pr_info("Share [%s] -> %s\n", shares[i].name, shares[i].lkl_path);
    ```

  - replace the `halt:` epilogue's close-loop + `anyfs_kernel_halt();` with `anyfs_server_shutdown(disks, n_images);`.
- [ ] **Step 5: Build + verify** — `./scripts/build_anyfs.sh --targets=linux-amd64 --components=core,server -j"$(nproc)"` → clean.
- [ ] **Step 6: Commit** — `git commit -m "refactor(ksmbd): use server_common skeleton"`

### Task 12: Port nfsd_main.c to server_common + smoke both servers

**Files:**
- Modify: `src/nfsd/nfsd_main.c`

- [ ] **Step 1:** Add `#include "server_common.h"`; delete local `running`/`sigint_handler`; replace `while (running)` with `while (anyfs_server_running)`.
- [ ] **Step 2:** In `main()`: replace the signal/setbuf triple with `anyfs_server_install_signals();`; replace the kernel init + `lkl_if_up(1)` block with `anyfs_server_boot(&kern_opts)` (same pattern as Task 11, keeping nfsd's own `printf("LKL kernel started (nfsd built-in)\n");`).
- [ ] **Step 3:** Replace the export-resolve loop. `ExportInfo` keeps its `bind_path` member, so resolve into shared entries first, then copy:

```c
	/* ── 5. Resolve --share specs to LKL paths ───────────────────────── */
	{
		AnyfsShareEntry ents[ANYFS_MAX_SHARES];
		uint32_t eflags = read_only ? ANYFS_SESSION_READONLY : 0;
		int n = anyfs_server_resolve_shares(share_specs, n_share_specs,
						    disks, n_images, eflags,
						    ents, ANYFS_MAX_SHARES);
		if (n < 0)
			goto halt;
		for (int i = 0; i < n; i++) {
			ExportInfo* ex = &g_exports[g_n_exports];
			memcpy(ex->name, ents[i].name, sizeof(ex->name));
			memcpy(ex->lkl_path, ents[i].lkl_path,
			       sizeof(ex->lkl_path));
			printf("Export [%d] /%s -> %s\n", g_n_exports, ex->name,
			       ex->lkl_path);
			g_n_exports++;
		}
	}
```

(`ExportInfo.name` is `char[64]` and `lkl_path` is `char[ANYFS_LKL_PATH_MAX]` — identical sizes, so `memcpy` of the full member is safe.)
- [ ] **Step 4:** Replace the `halt:` close-loop + `anyfs_kernel_halt();` with `anyfs_server_shutdown(disks, n_images);` (keep the final `printf("Done\n"); return 0;`).
- [ ] **Step 5: Build** — same build_anyfs.sh invocation → clean.
- [ ] **Step 6: Behavior verification — run the integration smoke test against the local Debian image:**

Run: `./tests/smoke-debian-qcow2.sh --build-dir="$PWD/build-anyfs-linux-amd64" "$PWD/debian.qcow2"`
Expected: lspart + smbclient + NFS checks PASS (no `--nfs-mount` locally — that needs sudo). Ctrl+C handling is covered by the script's own teardown.
- [ ] **Step 7: Commit** — `git commit -m "refactor(nfsd): use server_common skeleton"`

---

## Phase 4 — C and TS unit tests

### Task 13: C unit suite — path DSL (TDD scaffolding for `meson test`)

**Files:**
- Create: `tests/unit/test_path_dsl.c`
- Modify: `meson.build` (after the existing `# --- Tests ---` section)

- [ ] **Step 1: Write the test**

`tests/unit/test_path_dsl.c`:

```c
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the path DSL parser (src/core/anyfs_path.c).
 * Pure userspace — no LKL boot, runs in milliseconds under `meson test`. */
#include "anyfs_path.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

static void test_simple(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("p1", &ap) == 0);
	CHECK(ap.n_comp == 1);
	CHECK(ap.comp[0].p == 1);
	CHECK(ap.comp[0].query == NULL);
	CHECK(ap.disk_idx_set == 0);
	anyfs_path_free(&ap);
}

static void test_disk_prefix(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("disk0/p1", &ap) == 0);
	CHECK(ap.disk_idx_set == 1);
	CHECK(ap.disk_idx == 0);
	CHECK(ap.n_comp == 1 && ap.comp[0].p == 1);
	anyfs_path_free(&ap);

	CHECK(anyfs_path_parse("disk12/p2/p1", &ap) == 0);
	CHECK(ap.disk_idx == 12);
	CHECK(ap.n_comp == 2);
	CHECK(ap.comp[0].p == 2 && ap.comp[1].p == 1);
	anyfs_path_free(&ap);
}

static void test_slashes_and_case(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("/p1/", &ap) == 0); /* leading+trailing ok */
	CHECK(ap.n_comp == 1 && ap.comp[0].p == 1);
	anyfs_path_free(&ap);

	CHECK(anyfs_path_parse("P3", &ap) == 0); /* uppercase P accepted */
	CHECK(ap.comp[0].p == 3);
	anyfs_path_free(&ap);
}

static void test_query(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("p1?keyref=LUKS_KEY", &ap) == 0);
	CHECK(ap.comp[0].query != NULL);
	CHECK(strcmp(ap.comp[0].query, "keyref=LUKS_KEY") == 0);
	anyfs_path_free(&ap);

	/* Query is percent-decoded in place; %2F must not re-split. */
	CHECK(anyfs_path_parse("p1?keyfile=%2Ftmp%2Fk", &ap) == 0);
	CHECK(strcmp(ap.comp[0].query, "keyfile=/tmp/k") == 0);
	anyfs_path_free(&ap);

	/* Bad escape in query is a parse error. */
	CHECK(anyfs_path_parse("p1?key=%zz", &ap) == -1);
}

static void test_errors(void)
{
	AnyfsPath ap;
	CHECK(anyfs_path_parse("", &ap) == -1);
	CHECK(anyfs_path_parse("/", &ap) == -1);
	CHECK(anyfs_path_parse("p0", &ap) == -1);   /* index must be > 0 */
	CHECK(anyfs_path_parse("p", &ap) == -1);
	CHECK(anyfs_path_parse("x1", &ap) == -1);
	CHECK(anyfs_path_parse("disk0", &ap) == -1); /* disk alone invalid */
	CHECK(anyfs_path_parse("disk/p1", &ap) == -1);
	CHECK(anyfs_path_parse("diskX/p1", &ap) == -1);
	CHECK(anyfs_path_parse("p1x", &ap) == -1);
	/* 9 components > ANYFS_PATH_MAX_COMPONENTS (8) */
	CHECK(anyfs_path_parse("p1/p1/p1/p1/p1/p1/p1/p1/p1", &ap) == -1);
	CHECK(anyfs_path_parse(NULL, &ap) == -1);
}

static void test_pct_decode(void)
{
	char a[] = "a%2Fb";
	CHECK(anyfs_path_pct_decode(a) == 0);
	CHECK(strcmp(a, "a/b") == 0);

	char b[] = "plain";
	CHECK(anyfs_path_pct_decode(b) == 0);
	CHECK(strcmp(b, "plain") == 0);

	char c[] = "%zz";
	CHECK(anyfs_path_pct_decode(c) == -1);

	char d[] = "%4"; /* truncated escape */
	CHECK(anyfs_path_pct_decode(d) == -1);

	CHECK(anyfs_path_pct_decode(NULL) == 0);
}

int main(void)
{
	test_simple();
	test_disk_prefix();
	test_slashes_and_case();
	test_query();
	test_errors();
	test_pct_decode();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_path_dsl: all OK\n");
	return 0;
}
```

- [ ] **Step 2: Register in meson.** In `meson.build`, after the existing tests block (`test_nfsd_pseudo_root = ...`), add:

```meson
# --- Unit tests (pure userspace, no LKL boot; run via `meson test --suite unit`) ---
test_path_dsl = executable('test_path_dsl',
    'tests/unit/test_path_dsl.c',
    'src/core/anyfs_path.c',
    include_directories: [anyfs_inc],
    install: false,
)
test('path_dsl', test_path_dsl, suite: 'unit')
```

(`anyfs_path.c` is freestanding — no LKL/glib deps — so the test links in milliseconds.)

- [ ] **Step 3: Run to verify it fails… then passes.** First deliberately check the harness catches failures: temporarily flip one assertion (e.g. `CHECK(ap.comp[0].p == 2)` in `test_simple`), run, see FAIL; revert. Then:

Run: `ninja -C build-anyfs-linux-amd64 && meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs`
Expected: `1/1 path_dsl OK`.

- [ ] **Step 4: Commit** — `git add tests/unit meson.build && git commit -m "test(core): meson unit suite + path DSL parser tests"`

### Task 14: C unit suite — share helpers

**Files:**
- Create: `tests/unit/test_share_helpers.c`
- Modify: `meson.build` (append below the path_dsl test)

- [ ] **Step 1: Write the test**

```c
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the pure --share helpers (src/core/anyfs_share.c). */
#include "anyfs_share.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

static void test_auto_name(void)
{
	char out[32];
	anyfs_share_auto_name("disk0/p1", out, sizeof(out));
	CHECK(strcmp(out, "disk0_p1") == 0);

	anyfs_share_auto_name("p2", out, sizeof(out));
	CHECK(strcmp(out, "p2") == 0);

	/* Truncation: output is always NUL-terminated. */
	char tiny[5];
	anyfs_share_auto_name("disk0/p1", tiny, sizeof(tiny));
	CHECK(strcmp(tiny, "disk") == 0);

	/* out_sz == 0 must not write anything (no crash). */
	anyfs_share_auto_name("x", tiny, 0);
}

static void test_split(void)
{
	const char *name, *path;

	char a[] = "data=disk0/p1";
	CHECK(anyfs_share_split(a, &name, &path) == 0);
	CHECK(name && strcmp(name, "data") == 0);
	CHECK(strcmp(path, "disk0/p1") == 0);

	char b[] = "disk0/p1";
	CHECK(anyfs_share_split(b, &name, &path) == 0);
	CHECK(name == NULL);
	CHECK(strcmp(path, "disk0/p1") == 0);

	/* Only the FIRST '=' splits; the rest stays in path. */
	char c[] = "n=p1?key=v";
	CHECK(anyfs_share_split(c, &name, &path) == 0);
	CHECK(strcmp(name, "n") == 0);
	CHECK(strcmp(path, "p1?key=v") == 0);
}

int main(void)
{
	test_auto_name();
	test_split();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_share_helpers: all OK\n");
	return 0;
}
```

- [ ] **Step 2: Register in meson** (anyfs_share.c calls into sessions, so link the full dep like `test_raw_mount` does):

```meson
test_share_helpers = executable('test_share_helpers',
    'tests/unit/test_share_helpers.c',
    include_directories: [anyfs_inc],
    dependencies: [anyfs_dep],
    c_args: gio_args + qemu_args,
    install: false,
)
test('share_helpers', test_share_helpers, suite: 'unit')
```

- [ ] **Step 3: Run** — `meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs` → `2/2 OK`.
- [ ] **Step 4: Commit** — `git commit -m "test(core): share helper unit tests"`

### Task 15: CI — run the C unit suite + wasm-exports gate in linux.yml

**Files:**
- Modify: `.github/workflows/linux.yml`

- [ ] **Step 1:** After the `Build anyfs-reader (core + server + fuse)` step, add:

```yaml
      - name: Run C unit tests
        run: meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs

      - name: Wasm export drift gate
        run: ./tests/test_wasm_exports.sh
```

- [ ] **Step 2: Validate YAML** — `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/linux.yml'))"`
- [ ] **Step 3: Commit** — `git commit -m "ci(linux): run C unit suite + wasm export gate"`

### Task 16: TS unit tests — @anyfs/core (node:test, no wasm needed)

**Files:**
- Modify: `ts/packages/core/src/index.ts` (export `AnyfsSessionBase` if not already exported — check with `grep -n "session-base" ts/packages/core/src/index.ts`)
- Create: `ts/packages/core/test/format.test.mjs`
- Create: `ts/packages/core/test/session-base.test.mjs`
- Create: `ts/packages/core/test/dispatch.test.mjs` (supersedes `dispatch.test.ts`, which is currently not executed by any script)
- Delete: `ts/packages/core/test/dispatch.test.ts`
- Modify: `ts/packages/core/package.json` (scripts)

- [ ] **Step 1:** Ensure exports. In `ts/packages/core/src/index.ts` add (if missing):

```ts
export { AnyfsSessionBase } from './session-base.js';
```

Verify `createSession` and the `format.ts` helpers (`fmtBytes`, `fmtMode`, `splitExt`, `formatSize`) are exported from the index; add `export * from './format.js';` if absent.

- [ ] **Step 2:** Write `ts/packages/core/test/format.test.mjs` (runs against built `dist/`):

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fmtBytes, fmtMode, splitExt, formatSize } from '../dist/index.js';

test('fmtBytes tiers', () => {
    assert.equal(fmtBytes(512), '512 B');
    assert.equal(fmtBytes(2048), '2048 B (2.0 KiB)');
    assert.match(fmtBytes(5 * 1024 * 1024), /5\.0 MiB/);
    assert.match(fmtBytes(3 * 1024 ** 3), /3\.00 GiB/);
});

test('fmtMode common cases', () => {
    assert.equal(fmtMode(0o100644), '-rw-r--r-- (0644)');
    assert.equal(fmtMode(0o040755), 'drwxr-xr-x (0755)');
    assert.equal(fmtMode(0o120777), 'lrwxrwxrwx (0777)');
    assert.equal(fmtMode(0o104755), '-rwsr-xr-x (4755)'); // setuid
    assert.equal(fmtMode(0o041777), 'drwxrwxrwt (1777)'); // sticky /tmp
});

test('splitExt — Chonky-bug-safe rules', () => {
    assert.equal(splitExt('a.txt'), '.txt');
    assert.equal(splitExt('noext'), '');          // no dot -> ''
    assert.equal(splitExt('.bashrc'), '');        // dotfile -> ''
    assert.equal(splitExt('.pwd.lock'), '.lock'); // later dot splits
    assert.equal(splitExt('trailing.'), '');      // trailing dot -> ''
});

test('formatSize adaptive units', () => {
    assert.equal(formatSize(undefined), '');
    assert.equal(formatSize(0), '0 B');
    assert.equal(formatSize(1536), '1.5 KiB');
    assert.equal(formatSize(10 * 1024 * 1024), '10 MiB');
});
```

- [ ] **Step 3:** Write `ts/packages/core/test/session-base.test.mjs` — a fake in-memory session exercising the shared base-class logic:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AnyfsSessionBase } from '../dist/index.js';

/** In-memory tree: dirs map path -> entries; files map path -> bytes. */
class FakeSession extends AnyfsSessionBase {
    constructor({ dirs = {}, files = {}, symlinkSizes = {} } = {}) {
        super();
        this.dirs = dirs;
        this.files = files;
        this.symlinkSizes = symlinkSizes; // lstat-size != follow-size repro
        this.openCount = 0;
        this.closedFds = [];
        this.readdirCalls = 0;
    }
    async attachBlob() {} async attachUrl() {} async attachPath() {}
    async enter() { return '/mnt'; }
    async listParts() { return []; }
    async meta() { return {}; }
    async readdir(path) {
        this.readdirCalls++;
        const e = this.dirs[path];
        if (!e) throw new Error(`ENOENT ${path}`);
        return e;
    }
    async stat(path) {
        if (path in this.symlinkSizes) return { size: this.symlinkSizes[path], mode: 0o120777 };
        return { size: this.files[path]?.length ?? 0, mode: 0o100644 };
    }
    async statFollow(path) { return { size: this.files[path]?.length ?? 0, mode: 0o100644 }; }
    async readlink() { return ''; }
    async realpath(p) { return p; }
    async readKernelFile() { return ''; }
    onProgress() { return () => {}; }
    async _openFdRaw() { this.openCount++; return this.openCount; }
    async _readFdRaw(fd, offset, length) {
        const bytes = this.currentBytes;
        return bytes.subarray(offset, offset + length);
    }
    async _closeFdRaw(fd) { this.closedFds.push(fd); }
    async _dispose() { this.disposedCalled = true; }
}

async function drain(stream) {
    const chunks = [];
    for await (const c of stream) chunks.push(c);
    return chunks;
}

test('openReadable sizes from statFollow, not lstat (symlink truncation bug)', async () => {
    const data = new Uint8Array(100).fill(7);
    const s = new FakeSession({
        files: { '/etc/os-release': data },
        symlinkSizes: { '/etc/os-release': 21 }, // lstat sees link-target length
    });
    s.currentBytes = data;
    const { stream, size } = await s.openReadable('/etc/os-release');
    assert.equal(size, 100); // would be 21 if base used stat()
    const chunks = await drain(stream);
    assert.equal(chunks.reduce((n, c) => n + c.length, 0), 100);
});

test('openReadable chunks and closes the fd exactly once', async () => {
    const data = new Uint8Array(2500).fill(1);
    const s = new FakeSession({ files: { '/f': data } });
    s.currentBytes = data;
    const { stream } = await s.openReadable('/f', { chunkSize: 1000 });
    const chunks = await drain(stream);
    assert.deepEqual(chunks.map((c) => c.length), [1000, 1000, 500]);
    assert.deepEqual(s.closedFds, [1]);
});

test('openReadable cancel closes the fd', async () => {
    const data = new Uint8Array(5000).fill(1);
    const s = new FakeSession({ files: { '/f': data } });
    s.currentBytes = data;
    const { stream } = await s.openReadable('/f', { chunkSize: 1000 });
    const reader = stream.getReader();
    await reader.read();
    await reader.cancel();
    assert.deepEqual(s.closedFds, [1]);
});

test('walk is BFS, skips unreadable dirs, honors chunkSize', async () => {
    const s = new FakeSession({
        dirs: {
            '/': [
                { name: 'a', kind: 'dir' },
                { name: 'f1', kind: 'file' },
            ],
            '/a': [{ name: 'f2', kind: 'file' }, { name: 'bad', kind: 'dir' }],
            // '/a/bad' missing -> readdir throws -> silently skipped
        },
    });
    const seen = [];
    for await (const chunk of s.walk('/', 2)) seen.push(...chunk);
    assert.deepEqual(seen, ['/a', '/f1', '/a/f2', '/a/bad']);
});

test('close() closes tracked fds, disposes once, and poisons the session', async () => {
    const data = new Uint8Array(10);
    const s = new FakeSession({ files: { '/f': data } });
    s.currentBytes = data;
    await s.openFd('/f');
    await s.close();
    assert.deepEqual(s.closedFds, [1]);
    assert.equal(s.disposedCalled, true);
    await assert.rejects(() => s.openFd('/f'), /already disposed/);
    await s.close(); // idempotent
});
```

- [ ] **Step 4:** Write `ts/packages/core/test/dispatch.test.mjs` — full matrix, replacing the never-run `.ts` file:

```js
import { test, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { createSession } from '../dist/index.js';

afterEach(() => { delete globalThis.anyfsNative; });

const fakeBridge = { sessionOpen() {}, kernelInit() {} };

test('web → wasm, blob+url only, no bridge', () => {
    const r = createSession('web');
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.allowedKinds, new Set(['blob', 'url']));
    assert.equal(r.nativeBridge, undefined);
    assert.equal(typeof r.wasmCaps, 'object');
});

test('node → node-wasm, path only', () => {
    const r = createSession('node');
    assert.equal(r.backend, 'node-wasm');
    assert.deepEqual(r.allowedKinds, new Set(['path']));
});

test('electron + bridge → native, path+url', () => {
    globalThis.anyfsNative = fakeBridge;
    const r = createSession('electron');
    assert.equal(r.backend, 'native');
    assert.equal(r.nativeBridge, fakeBridge);
    assert.deepEqual(r.allowedKinds, new Set(['path', 'url']));
});

test('electron + bridge + disableNative → wasm fallback', () => {
    globalThis.anyfsNative = fakeBridge;
    const r = createSession('electron', { disableNative: true });
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.allowedKinds, new Set(['blob', 'url']));
});

test('electron without bridge → wasm fallback', () => {
    const r = createSession('electron');
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.allowedKinds, new Set(['blob', 'url']));
});

test('electron wasm + pathLoopbackUrl cap → path allowed', () => {
    const r = createSession('electron', {
        disableNative: true,
        electronWasmCaps: { pathLoopbackUrl: 'http://127.0.0.1:9999/d0' },
    });
    assert.equal(r.backend, 'wasm');
    assert.ok(r.allowedKinds.has('path'));
    assert.equal(r.wasmCaps.pathLoopbackUrl, 'http://127.0.0.1:9999/d0');
});

test('electron wasm caps forwarded verbatim (urlProxyPrefix)', () => {
    const r = createSession('electron', {
        disableNative: true,
        electronWasmCaps: { urlProxyPrefix: 'anyfs-url://proxy/?u=' },
    });
    assert.equal(r.wasmCaps.urlProxyPrefix, 'anyfs-url://proxy/?u=');
    assert.ok(!r.allowedKinds.has('path')); // no loopback cap -> no path
});
```

`rm ts/packages/core/test/dispatch.test.ts`

**Caveat for the executor:** if `getAnyfsNative()` caches or checks `window` instead of `globalThis`, read `ts/packages/core/src/native-session.ts:30-40` and adapt the global stubbing accordingly (verified 2026-06-10: it reads `globalThis.anyfsNative`).

- [ ] **Step 5:** Wire scripts. In `ts/packages/core/package.json`:

```json
"scripts": {
    "build": "tsup",
    "test:unit": "node --test test/*.test.mjs",
    "test": "npm run test:unit && node test/smoke.node.mjs single && node test/smoke.node.mjs multi && node test/smoke.node.mjs big"
}
```

- [ ] **Step 6: Run to verify they fail first** (before `pnpm build`, dist is stale and `AnyfsSessionBase` export missing): `cd ts && pnpm --filter @anyfs/core run test:unit` → expect import error. Then `pnpm --filter @anyfs/core build && pnpm --filter @anyfs/core run test:unit` → expect all PASS.
- [ ] **Step 7: Commit** — `git add ts/packages/core && git commit -m "test(core): node:test unit suite — format, session-base, dispatch matrix"`

### Task 17: TS unit tests — @anyfs/react (vitest + testing-library)

**Files:**
- Modify: `ts/packages/react/package.json`
- Create: `ts/packages/react/vitest.config.ts`
- Create: `ts/packages/react/test/provider.test.tsx`
- Create: `ts/packages/react/test/hooks.test.tsx`

- [ ] **Step 1:** Add dev deps + scripts in `ts/packages/react/package.json`:

```json
"scripts": {
    "build": "tsup src/index.ts --format esm --dts --clean --external react --external @anyfs/core",
    "test": "vitest run"
},
"devDependencies": {
    "typescript": "^5.5.4",
    "tsup": "^8.3.0",
    "@types/react": "^19.0.0",
    "react": "^19.0.0",
    "react-dom": "^19.0.0",
    "vitest": "^3.0.0",
    "jsdom": "^25.0.0",
    "@testing-library/react": "^16.1.0"
}
```

Run: `cd ts && pnpm install`

- [ ] **Step 2:** `ts/packages/react/vitest.config.ts`:

```ts
import { defineConfig } from 'vitest/config';

export default defineConfig({
    test: {
        environment: 'jsdom',
        include: ['test/**/*.test.tsx'],
        globals: false,
    },
});
```

- [ ] **Step 3:** Write `ts/packages/react/test/provider.test.tsx`. Mock `@anyfs/core` so the provider's state machine is tested without wasm/IPC:

```tsx
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import React from 'react';

const fakeSession = () => ({
    attachBlob: vi.fn(async () => {}),
    attachUrl: vi.fn(async () => {}),
    attachPath: vi.fn(async () => {}),
    onProgress: vi.fn(() => () => {}),
    close: vi.fn(async () => {}),
    readdir: vi.fn(async () => []),
    stat: vi.fn(async () => ({ size: 0, mode: 0o100644 })),
    openFd: vi.fn(async () => 3),
    readFd: vi.fn(async () => new Uint8Array(0)),
    closeFd: vi.fn(async () => {}),
});

const prewarmMock = vi.fn();

vi.mock('@anyfs/core', () => ({
    createSession: (env: string) => ({
        backend: 'wasm',
        allowedKinds: new Set(['blob', 'url']),
        wasmCaps: {},
    }),
    prewarm: (...args: unknown[]) => prewarmMock(...args),
    prewarmNative: vi.fn(),
    NativeSession: class NativeSessionMock {},
}));

import { AnyfsProvider, useAnyfsDisk } from '../src/index.js';

function Status() {
    const { status, error } = useAnyfsDisk();
    return <div data-testid="status">{status}{error ? `:${error.message}` : ''}</div>;
}

beforeEach(() => {
    prewarmMock.mockReset();
});

describe('AnyfsProvider', () => {
    it('idle without source or prewarm', () => {
        render(
            <AnyfsProvider source={null} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        expect(screen.getByTestId('status').textContent).toBe('idle');
    });

    it('prewarm → booting → booted', async () => {
        let resolve!: (s: unknown) => void;
        prewarmMock.mockReturnValue(new Promise((r) => (resolve = r)));
        render(
            <AnyfsProvider source={null} workerUrl="/w.js" prewarm>
                <Status />
            </AnyfsProvider>,
        );
        expect(screen.getByTestId('status').textContent).toBe('booting');
        resolve(fakeSession());
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('booted'));
    });

    it('blob source attaches and reaches ready', async () => {
        const s = fakeSession();
        prewarmMock.mockResolvedValue(s);
        const blob = new Blob([new Uint8Array(16)]);
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('ready'));
        expect(s.attachBlob).toHaveBeenCalledWith(blob);
    });

    it('disallowed source kind → error state', async () => {
        prewarmMock.mockResolvedValue(fakeSession());
        render(
            <AnyfsProvider source={{ kind: 'path', path: '/dev/sda' } as never} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(screen.getByTestId('status').textContent).toMatch(/^error:.*not supported/),
        );
    });

    it('prewarm failure → error state', async () => {
        prewarmMock.mockRejectedValue(new Error('boot failed'));
        render(
            <AnyfsProvider source={null} workerUrl="/w.js" prewarm>
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(screen.getByTestId('status').textContent).toBe('error:boot failed'),
        );
    });
});
```

- [ ] **Step 4:** Write `ts/packages/react/test/hooks.test.tsx`:

```tsx
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import React from 'react';

const prewarmMock = vi.fn();
vi.mock('@anyfs/core', () => ({
    createSession: () => ({ backend: 'wasm', allowedKinds: new Set(['blob', 'url']), wasmCaps: {} }),
    prewarm: (...args: unknown[]) => prewarmMock(...args),
    prewarmNative: vi.fn(),
    NativeSession: class NativeSessionMock {},
}));

import { AnyfsProvider, useAnyfsDir, useAnyfsFile } from '../src/index.js';

const entriesRoot = [
    { name: 'etc', kind: 'dir' },
    { name: 'README', kind: 'file' },
];

function makeSession() {
    return {
        attachBlob: vi.fn(async () => {}),
        onProgress: vi.fn(() => () => {}),
        close: vi.fn(async () => {}),
        readdir: vi.fn(async (p: string) => {
            if (p === '/') return entriesRoot;
            throw new Error('ENOENT');
        }),
        stat: vi.fn(async () => ({ size: 4, mode: 0o100644 })),
        openFd: vi.fn(async () => 7),
        readFd: vi.fn(async () => new Uint8Array([1, 2, 3, 4])),
        closeFd: vi.fn(async () => {}),
    };
}

function Tree({ path }: { path: string }) {
    const { entries, loading, error } = useAnyfsDir(path);
    if (error) return <div data-testid="dir">error:{error.message}</div>;
    if (loading || !entries) return <div data-testid="dir">loading</div>;
    return <div data-testid="dir">{entries.map((e) => e.name).join(',')}</div>;
}

function FileBytes({ path }: { path: string }) {
    const { data, error } = useAnyfsFile(path);
    if (error) return <div data-testid="file">error</div>;
    return <div data-testid="file">{data ? Array.from(data).join(',') : 'loading'}</div>;
}

const blob = new Blob([new Uint8Array(8)]);

beforeEach(() => prewarmMock.mockReset());

describe('useAnyfsDir', () => {
    it('lists entries once ready and caches per (session,path)', async () => {
        const s = makeSession();
        prewarmMock.mockResolvedValue(s);
        const { rerender } = render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Tree path="/" />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('dir').textContent).toBe('etc,README'));
        // Remount the consumer — cache must serve it without a second readdir.
        rerender(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Tree path="/" />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('dir').textContent).toBe('etc,README'));
        expect(s.readdir).toHaveBeenCalledTimes(1);
    });

    it('surfaces readdir errors', async () => {
        prewarmMock.mockResolvedValue(makeSession());
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Tree path="/missing" />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(screen.getByTestId('dir').textContent).toBe('error:ENOENT'),
        );
    });
});

describe('useAnyfsFile', () => {
    it('stat + openFd + readFd + closeFd round trip', async () => {
        const s = makeSession();
        prewarmMock.mockResolvedValue(s);
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <FileBytes path="/README" />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('file').textContent).toBe('1,2,3,4'));
        expect(s.openFd).toHaveBeenCalledWith('/README');
        expect(s.readFd).toHaveBeenCalledWith(7, 0, 4);
        expect(s.closeFd).toHaveBeenCalledWith(7);
    });
});
```

- [ ] **Step 5: Run to verify red → green.** `cd ts && pnpm --filter @anyfs/react test` — first run may fail on mock-shape mismatches with the real provider; fix the **tests** (the provider is the spec here), not the provider, unless the failure exposes a real provider bug — in that case stop and report it before changing provider code.
- [ ] **Step 6:** Confirm the workspace-level `pnpm test` (`pnpm -r --filter './packages/*' test`) now runs core unit + smoke, react vitest, and the existing nbd-proxy suite. Run from `ts/`: `pnpm test` → all green.
- [ ] **Step 7: Commit** — `git add ts/packages/react ts/pnpm-lock.yaml && git commit -m "test(react): vitest provider + hooks unit suite"`

### Task 18: CI — TS unit workflow

**Files:**
- Create: `.github/workflows/ts.yml`

- [ ] **Step 1:**

```yaml
name: ts

# Pure TS unit tests (no wasm bundle, no LKL): @anyfs/core node:test suite +
# @anyfs/react vitest suite + @anyfs/nbd-proxy. The wasm smoke tests
# (test/smoke.node.mjs) need a built bundle and stay local/linux-job-only.

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
  workflow_dispatch:

jobs:
  unit:
    runs-on: ubuntu-24.04
    timeout-minutes: 15
    steps:
      - uses: actions/checkout@v4
      - uses: pnpm/action-setup@v4
        with:
          package_json_file: ts/package.json
      - uses: actions/setup-node@v4
        with:
          node-version: 24
          cache: pnpm
          cache-dependency-path: ts/pnpm-lock.yaml
      - name: Install
        working-directory: ts
        run: pnpm install --frozen-lockfile
      - name: Build packages
        working-directory: ts
        run: pnpm -r --filter './packages/*' build
      - name: Core unit tests
        working-directory: ts
        run: pnpm --filter @anyfs/core run test:unit
      - name: React unit tests
        working-directory: ts
        run: pnpm --filter @anyfs/react test
      - name: nbd-proxy tests
        working-directory: ts
        run: pnpm --filter @anyfs/nbd-proxy test
```

**Caveat:** `@anyfs/anyfs-native` build (node-gyp) may fail or be skipped on CI — if `pnpm -r build` chokes on it, exclude it: `pnpm -r --filter './packages/*' --filter '!@anyfs/anyfs-native' build`. Check `ts/packages/anyfs-native/package.json`'s build script first.

- [ ] **Step 2:** Validate: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ts.yml'))"`
- [ ] **Step 3: Commit** — `git commit -m "ci: ts unit-test workflow"`

### Task 18A: CDP↔Playwright parity audit, then demote the CDP suite to diagnostics

**Files:**
- Create: `tests/diagnostics/README.md`
- Move: `tests/{test-cdp,run-all,common-cdp}.mjs` → `tests/diagnostics/`
- Possibly create: new specs under `ts/tests/e2e/flows/` (only where the audit finds gaps)

Principle (user decision): temporary tests are regression guards — nothing with assertion value gets deleted; the CDP suite is demoted, not removed, once Playwright parity is confirmed.

- [ ] **Step 1: Build the parity matrix.** Read `tests/test-cdp.mjs` and list every assertion per combo (6 combos: web/electron-wasm/electron-native × local/http). Map each to the Playwright coverage: projects `web`/`electron-wasm`/`electron-native` × specs `open-browse-download` (local open + browse + download), `url-load` (URL incl. local Range server), `formats`, `errors`, `electron-only/backend-switch`. Write the matrix as a table into `tests/diagnostics/README.md`.
- [ ] **Step 2:** For every CDP assertion with NO Playwright equivalent (expected candidates: any DOM detail test-cdp checks that the flows skip — e.g. specific tree-row interactions), add it to the matching existing spec in `ts/tests/e2e/flows/` rather than creating new files. Run the touched specs: `cd ts/tests/e2e && pnpm exec playwright test --project=web <spec>` → PASS.
- [ ] **Step 3:** `git mv tests/test-cdp.mjs tests/run-all.mjs tests/common-cdp.mjs tests/diagnostics/` and finish the README: state that Playwright (`ts/tests/e2e`) is the primary regression suite, these are manual diagnostics, and include the parity matrix + how to run (`node tests/diagnostics/run-all.mjs --image ...`). Fix the `TEST_SCRIPT` relative path inside `run-all.mjs` (it resolves `test-cdp.mjs` via `__dirname` — moving the files together keeps it valid; verify by running `node tests/diagnostics/run-all.mjs --help || true` and checking it locates the script).
- [ ] **Step 4: Commit** — `git commit -m "test: CDP suite demoted to diagnostics after Playwright parity audit"`

### Task 18B: Relocate test-core.mjs + corral the debug scripts

**Files:**
- Move: `tests/test-core.mjs`, `tests/common.mjs` → `ts/tests/integration/`
- Move: `tests/{test-atomics,test-async-boot,test-prewarm-direct,test-prewarm-e2e,test-direct-module,test-worker-debug}.mjs` → `tests/diagnostics/`
- Modify: `ts/package.json`, `tests/diagnostics/README.md`

- [ ] **Step 1:** `git mv tests/test-core.mjs tests/common.mjs ts/tests/integration/`. Fix any relative imports/paths inside them (`grep -n "\.\./" ts/tests/integration/*.mjs` — common.mjs path references to wasm bundles / fixtures must be updated for the new depth). Add to `ts/package.json` scripts:

```json
"test:integration": "node tests/integration/test-core.mjs"
```

Run it: `cd ts && pnpm run test:integration` → the 4 combos (native/wasm × file/url) PASS locally (requires built wasm bundle + native addon; this script is local-only, not wired into CI).
- [ ] **Step 2:** `git mv` the six debug scripts into `tests/diagnostics/`; extend the README with one line per script stating which regression it guards (atomics = Atomics.wait-on-main-thread regressions; async-boot = workeronly async boot path; prewarm-direct/e2e = landing prewarm; direct-module = main-page-load failure mode, expected to fail by design; worker-debug = worker boot logging). Verify each still parses: `for f in tests/diagnostics/*.mjs; do node --check "$f"; done`.
- [ ] **Step 3:** Confirm `tests/` now contains only: C test sources, `images/`, `setup.sh`, `run_tests.sh`, `smoke-debian-qcow2.sh`, `test_wasm_exports.sh`, `unit/`, `diagnostics/`.
- [ ] **Step 4: Commit** — `git commit -m "test: relocate test-core to ts integration; corral debug scripts under tests/diagnostics"`

---

## Phase 5 — sccache-dist-action in linux CI

**Prerequisite (manual, one-time):** repo secret `TS_OAUTH_SECRET` must exist on `xdqi/anyfs` — a Tailscale OAuth client secret with `auth_keys` write scope tagged `tag:ci-sccache` (already provisioned for the distcc/sccache projects; verify with `gh secret list -R xdqi/anyfs`). If absent, the farm steps are skipped and CI builds locally — no breakage.

### Task 19: `--cc` plumb-through for build_lkl.sh and build_qemu.sh

**Files:**
- Modify: `scripts/build_lkl.sh`
- Modify: `scripts/build_qemu.sh`

- [ ] **Step 1:** `build_lkl.sh`: add to the options help text `#   --cc=CMD            C compiler override passed to make as CC= (e.g. "sccache gcc")`, add `CC_OVERRIDE=""` next to the other defaults, add the case arm:

```bash
        --cc=*)      CC_OVERRIDE="${1#--cc=}"; shift ;;
        --cc)        CC_OVERRIDE="$2"; shift 2 ;;
```

and in `build_one()` extend the make invocations:

```bash
    local cc_arg=()
    [[ -n "$CC_OVERRIDE" ]] && cc_arg=(CC="$CC_OVERRIDE")
```

then append `"${cc_arg[@]}"` to both `make` calls (clean + build), after `"${cross_arg[@]}"`.

- [ ] **Step 2:** `build_qemu.sh`: same `--cc=` option parsing into `CC_OVERRIDE=""`. In the configure invocation (`"$QEMU_SRC/configure" ...` around line 225), prepend:

```bash
    local cc_cfg=()
    [[ -n "$CC_OVERRIDE" && "$target" == "linux-amd64" ]] && cc_cfg=("--cc=$CC_OVERRIDE")
```

and pass `"${cc_cfg[@]}"` to configure. (mingw targets keep their cross_prefix configure untouched — out of scope.) Note: `--cc` only takes effect on a fresh configure; document in the help text that `--reconfigure` is needed to switch compilers.

- [ ] **Step 3: Verify** — `bash -n` both; `./scripts/build_lkl.sh --help | grep -- --cc` shows the new option. Quick functional check: `./scripts/build_lkl.sh --targets=linux-amd64 --cc=gcc -j"$(nproc)"` → builds (no-op rebuild, CC=gcc is the default compiler anyway).
- [ ] **Step 4: Commit** — `git commit -m "feat(build): --cc compiler override for LKL and QEMU builds"`

### Task 20: Wire the farm into linux.yml

**Files:**
- Modify: `.github/workflows/linux.yml`

- [ ] **Step 1:** Add the worker fleet as a sibling job (top-level under `jobs:`):

```yaml
  # Ephemeral sccache-dist compile farm. Workers serve the coordinator
  # (the build job) and exit when it goes offline. Skipped on PRs —
  # fork PRs have no TS_OAUTH_SECRET and same-repo PRs don't need the
  # farm for cache-warm builds.
  sccache-workers:
    name: sccache worker ${{ matrix.idx }}
    if: ${{ github.event_name != 'pull_request' }}
    runs-on: ubuntu-24.04
    timeout-minutes: 75
    strategy:
      matrix:
        idx: [1, 2]
    steps:
      - uses: xdqi/sccache-dist-action@v0.0.4
        with:
          mode: worker
          worker-index: '${{ matrix.idx }}'
          oauth-secret: '${{ secrets.TS_OAUTH_SECRET }}'
```

- [ ] **Step 2:** In the `build` job, right after `Lint — no hardcoded paths...`, add:

```yaml
      # ── sccache-dist farm (best-effort; build proceeds locally if absent) ──
      - name: Cache sccache objects
        if: ${{ github.event_name != 'pull_request' }}
        uses: actions/cache@v4
        with:
          path: ~/.cache/sccache
          key: sccache-linux-amd64-${{ github.sha }}
          restore-keys: |
            sccache-linux-amd64-

      - name: Bring up sccache-dist farm
        if: ${{ github.event_name != 'pull_request' }}
        continue-on-error: true
        uses: xdqi/sccache-dist-action@v0.0.4
        with:
          mode: coordinator
          expected-workers: 2
          min-workers: 1
          wait-timeout: 180s
          oauth-secret: '${{ secrets.TS_OAUTH_SECRET }}'
```

- [ ] **Step 3:** Make the two heavy build steps farm-aware. Replace the `Build LKL kernel` run block's make invocation with:

```yaml
      - name: Build LKL kernel
        run: |
          CC_ARGS=()
          if command -v sccache >/dev/null 2>&1; then
            CC_ARGS=(--cc="sccache gcc")
          fi
          ./scripts/build_lkl.sh \
            --linux=deps/linux \
            --out="$GITHUB_WORKSPACE" \
            --targets=linux-amd64 \
            "${CC_ARGS[@]}" \
            -j"${SCCACHE_J:-$(nproc)}"
          mkdir -p deps/linux/tools/lkl/lib
          ln -sf "$GITHUB_WORKSPACE/lkl-linux-amd64/tools/lkl/lib/liblkl.so" \
                 deps/linux/tools/lkl/lib/liblkl.so
```

and the `Build QEMU block layer` step likewise:

```yaml
      - name: Build QEMU block layer
        run: |
          CC_ARGS=()
          if command -v sccache >/dev/null 2>&1; then
            CC_ARGS=(--cc="sccache gcc")
          fi
          ./scripts/build_qemu.sh \
            --qemu-src=deps/qemu \
            --targets=linux-amd64 \
            "${CC_ARGS[@]}" \
            -j"${SCCACHE_J:-$(nproc)}"
```

(`command -v sccache` is the farm-up probe: the coordinator action puts `sccache` on PATH and exports `SCCACHE_J`; on PRs or farm failure neither exists and the build runs exactly as today. `SCCACHE_DIST_FALLBACK=true` — the action default — keeps non-distributable jobs local.)

- [ ] **Step 4:** After the build steps, add stats for observability:

```yaml
      - name: sccache stats
        if: ${{ github.event_name != 'pull_request' }}
        continue-on-error: true
        run: sccache --show-stats || true
```

- [ ] **Step 5:** Validate YAML (`python3 -c "import yaml; yaml.safe_load(open('.github/workflows/linux.yml'))"`); commit — `git commit -m "ci(linux): sccache-dist compile farm (2 workers, best-effort)"`

### Task 20A: Wire the farm into mingw64.yml (experimental)

**Files:**
- Modify: `.github/workflows/mingw64.yml`

This is the unproven leg: sccache-dist must package the msys2-cross `x86_64-w64-mingw32-gcc` toolchain (under `/opt/msys2-cross`) and ship it to workers. The engine supports arbitrary-toolchain packaging, and the same fork engine already distributes zig cross-compiles for the msys2-cross triples — but plain cross-gcc with its sysroot has not been exercised. Everything is best-effort: on any farm failure the build falls back to local compilation exactly as today.

- [ ] **Step 1:** Add an `sccache-workers` job to `mingw64.yml` — identical shape to linux.yml's (matrix `idx: [1, 2]`, `if: github.event_name != 'pull_request'`).
- [ ] **Step 2:** In the build job, after the msys2-cross install step, add the same `Cache sccache objects` (key `sccache-mingw64-...`) + `Bring up sccache-dist farm` (coordinator, `continue-on-error: true`) steps as Task 20 Step 2.
- [ ] **Step 3:** Make the LKL kernel step farm-aware — same probe pattern, cross compiler spelled out:

```yaml
          CC_ARGS=()
          if command -v sccache >/dev/null 2>&1; then
            CC_ARGS=(--cc="sccache /opt/msys2-cross/bin/x86_64-w64-mingw32-gcc")
          fi
          ./scripts/build_lkl.sh ... --targets=mingw64 "${CC_ARGS[@]}" -j"${SCCACHE_J:-$(nproc)}"
```

(Absolute compiler path so sccache hashes/ships the right toolchain. QEMU's mingw configure is left alone — cross_prefix plumbing there is out of scope.)
- [ ] **Step 4:** Add the `sccache stats` step. Validate YAML, commit — `git commit -m "ci(mingw64): experimental sccache-dist farm for the LKL cross build"`
- [ ] **Step 5 (assessment gate):** After Task 21's run, check this job's stats: if cross compiles show `dist errors` / all-local fallback, file the finding in `docs/ci-sccache.md` ("mingw64 distribution blocked on X") and leave the wiring in place — it costs nothing when it falls back.

### Task 21: Validate on real Actions + document

**Files:**
- Create: `docs/ci-sccache.md`

- [ ] **Step 1:** Push the branch and trigger: `git push -u origin build-and-test-hardening && gh workflow run linux.yml --ref build-and-test-hardening` (workflow_dispatch is enabled; if `gh workflow run` rejects a non-default-branch workflow, open a draft PR instead — the `pull_request` event skips the farm but still validates the C unit tests, wasm gate, and that the farm-less fallback path builds green).
- [ ] **Step 2:** Watch: `gh run watch` — for **both** linux.yml and mingw64.yml assert (a) workers register, (b) the LKL build log shows the sccache-wrapped compiler, (c) `sccache --show-stats` shows non-zero distributed/cached compiles (mingw64 may legitimately show all-local fallback — record it per Task 20A Step 5), (d) artifacts still upload. Known wrinkle from the sccache-dist deployments: the v0.0.4 engine has **no S3 feature** — irrelevant here, the cache is local disk + actions/cache.
- [ ] **Step 3:** Write `docs/ci-sccache.md` documenting: required secret (`TS_OAUTH_SECRET`, Tailscale OAuth client with `auth_keys` write + `tag:ci-sccache`, needed on both `xdqi/anyfs` and `xdqi/llvm-wasm`), farm topology (2 workers + coordinator-in-build-job per workflow; 3 workers for llvm-wasm releases), the `command -v sccache` fallback contract, why PRs skip the farm, and the mingw64 cross-distribution status from Task 20A's assessment.
- [ ] **Step 4: Commit** — `git add docs/ci-sccache.md && git commit -m "docs: sccache-dist CI farm setup + fallback contract"`

---

## Final verification (whole plan)

- [ ] `./scripts/lint-no-hardcoded-paths.sh` + `./scripts/lint-shellcheck.sh` — PASS with every build script in the allowlists (`build_anyfs_browser_wasm.sh` is deleted).
- [ ] `./tests/test_wasm_exports.sh` — PASS.
- [ ] `./scripts/fetch_wasm_ld.sh && ./scripts/fetch_wasm_sysroot.sh && ./scripts/doctor.sh` — wasm-ld + sysroot sections OK from fetched artifacts alone.
- [ ] `meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs` — PASS.
- [ ] `./tests/smoke-debian-qcow2.sh --build-dir="$PWD/build-anyfs-linux-amd64" "$PWD/debian.qcow2"` — PASS.
- [ ] `cd ts && pnpm test` — PASS (core unit+smoke, react, nbd-proxy); `pnpm run test:integration` PASS locally.
- [ ] `cd ts/tests/e2e && pnpm exec playwright test` — PASS (incl. any specs added by the parity audit).
- [ ] linux.yml + mingw64.yml + ts.yml green on GitHub Actions; `wasm-ld-18.1.2-anyfs-r1` and `wasm-sysroot-r1` releases exist and are consumed.
- [ ] Use superpowers:requesting-code-review, then superpowers:finishing-a-development-branch.
