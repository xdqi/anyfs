# Linux zig cc / glibc floor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build every linux-amd64 artifact with zig cc against a pinned glibc floor
(2.11 for CLI tools and libraries, 2.25 for code loaded into Electron) and the x86-64
baseline ISA, with all third-party libraries linked statically from a private sysroot.

**Architecture:** A committed wrapper (`scripts/lib/zig-cc.sh`) turns `zig cc` into a
gcc-compatible compiler for a fixed target. `scripts/build_linux_sysroot.sh` builds
every dependency as a static `-fPIC` archive with it. LKL keeps gcc for the kernel half
(the whole kernel sub-make) and uses zig for tools/lkl's user-space half; QEMU and anyfs
build entirely with zig against the sysroot. `scripts/check_linux_abi.sh` gates every
shipped ELF.

**Tech Stack:** zig 0.16.0 (clang 21, lld), bash, meson/ninja, GNU make (kbuild),
node-gyp 11, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-05-linux-zig-glibc-floor-design.md`.

---

## Facts established by the spike (2026-10-05)

Six parallel spike builds under `~/.cache/anyfs-zig-spike/` proved each slice before
this plan was written. The facts below are why the code looks the way it does; do not
re-derive them.

### zig cc differs from gcc in ways the wrapper must undo

| zig behaviour | effect | wrapper response |
|---|---|---|
| `-dumpmachine` prints the versioned target, then errors (`version '.2.11' … is invalid`) | meson/autoconf can't detect the machine | print a plain GNU triple |
| `--print-search-dirs` prints the **host** gcc's library dirs (after the same bogus error) | meson `find_library()` linked `/usr/lib/x86_64-linux-gnu/libelf.so` into glib | print an empty list |
| `-O1` and above add `-DNDEBUG` | `assert()` vanishes everywhere; QEMU's osdep.h `#error`s | prepend `-UNDEBUG` |
| `-O0` / no `-O` enables UBSan (`-fsanitize=undefined`, panics on UB) | anyfs's meson default buildtype is debug | prepend `-fno-sanitize=undefined` |
| DWARF is emitted without `-g` | every archive carries debug info and the absolute build dir (libcrypto.a 35 MB vs 12 MB) | prepend `-g0` |
| `-Werror=date-time` | `__DATE__`/`__TIME__` fail to compile | nothing (no such use in our tree or deps) |

The three prepended flags come before the caller's arguments, so an explicit `-DNDEBUG`,
`-fsanitize=…` or `-g` from a build system still wins. The `.2.11 invalid` message itself
is a known harmless false error: ignore it unless a command actually fails.

Other zig facts:
- zig passes `-D__GLIBC_MINOR__=<floor>`, so `__GLIBC_PREREQ` is exact. A function newer
  than the floor is missing from zig's stub libraries (link error in an executable), but
  zig's headers still **declare** most of them, so compile-only probes
  (`has_header_symbol`, `AC_CHECK_DECL`, `cc.compiles`) report false positives. Link probes
  are correct.
- In a `-shared` link a missing function is **not** an error: it is left as an
  unversioned undefined symbol and only fails at load time. `check_linux_abi.sh` gates on
  it.
- zig adds the glibc stub libraries itself (`librt` for `clock_gettime` before 2.17,
  `libpthread`, `libdl`) with `--as-needed`.
- An explicit `-target` makes zig use the baseline x86-64 CPU (no AVX/SSE3/SSE4).
- Executables default to non-PIE and `-z now`; meson's `b_pie=true` still passes `-pie`.
- meson 1.7 detects the wrapper as `clang 21.1.0` with linker `ld.zigcc`.
- A static-only meson or autotools build writes its dependencies into the public
  `Libs`/`Requires` of its `.pc` file, so plain `pkg-config --libs` is complete and
  meson's `prefer_static` is not needed (and must not be used: with it `find_library()`
  only searches directories, and the wrapper reports none).
- lld resolves archives regardless of order; `--start-group` is unnecessary but harmless.
- msys2-cross's libc++ old-glibc patch (`patch_zig_libcxx_oldglibc.sh`) is only needed
  for C++ below glibc 2.16. The 2.11 side is pure C and the C++ addons target 2.25, so
  stock zig is used. If C++ ever moves to the 2.11 side, apply that patch to the zig
  install the way msys2-cross's `prepare-zig.sh` does.

### Per-library results (all static `-fPIC`, max GLIBC ≤ 2.10, NEEDED glibc-only)

| library | version | needed |
|---|---|---|
| zlib, bzip2, zstd, libffi | wasm pins | nothing |
| glib (+pcre2 subproject) | 2.88.0 | drop `HAVE_PTHREAD_GETNAME_NP` from `config.h` (2.12 symbol, header-probe false positive); `-Dlibelf=disabled` (host libelf leak) |
| util-linux libblkid | 2.40.4 tarball | `ac_cv_func___secure_getenv=no` (link probe finds the 2.2.5 compat symbol, headers no longer declare it); `-Dcrc32c=anyfs_blkid_crc32c` (clashes with QEMU's `crc32c` in libqemuutil.a: duplicate symbol, or VHDX/ext4 checksums silently wrong) |
| libaio | 0.3.113 | patch out `.symver` tags (a `-shared` link of `io_getevents` fails with "undefined version LIBAIO_0.4"); CFLAGS only through the environment; write `libaio.pc` |
| liburing | 2.15 | `--use-libc` (nolibc mode's `-nostdlib` makes zig drop libc headers); `-fPIC` in CFLAGS |
| libfuse | 3.18.3 | `aligned_alloc` (2.16) → `posix_memalign`; `-Ddisable-libc-symbol-version=true`; `--bindir=/usr/bin` (compiled-in fusermount3 path; glibc < 2.24 never reaches the PATH fallback) |
| OpenSSL | 3.5.9 | none; `--openssldir=/etc/ssl`, `no-shared no-module no-tests no-docs no-apps` |
| curl | 8.22.0 | none; keep FTP (QEMU's block/curl.c sets `CURLOPT_PROTOCOLS_STR "HTTP,HTTPS,FTP,FTPS"` and curl rejects the whole list if one is compiled out); `--disable-openssl-auto-load-config` (don't read the distro's openssl.cnf); no CA bundle (`--without-ca-bundle --without-ca-path --with-ca-fallback`) |

Every meson build of a dependency gets `cmake = 'false'` in its native file: meson's
cmake fallback otherwise finds host packages through `/usr/bin/cmake`.

### Electron addon at 2.25 (node-gyp 11.5, Electron 42.3.0)

- `CC`/`CXX` = the wrappers, `ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.25` in the environment;
  leave `LINK` unset so it falls back to `zig c++`, the driver that links libc++.
- libc++, libc++abi, libunwind and compiler-rt end up as local symbols: no
  `libstdc++.so.6`/`libgcc_s.so.1` in NEEDED. C++ exceptions work inside Electron.
  Electron's own libc++ (`std::__Cr`) is not exported, so the two never meet.
- `-fvisibility=hidden` + a wildcard version script cut the exports from 4127 to 2.
  zig rejects `-Wl,--exclude-libs`; lld errors on a version-script name that isn't
  defined, hence the wildcards.
- 2.11-built objects call `stat64`; for any target below 2.33 zig adds local wrappers to
  `__xstat64@GLIBC_2.2.5`, the same at 2.11 and 2.25.
- drivelist has no native Linux dependencies (Linux enumeration is the JS lsblk path);
  only libc++.

### Runtime notes

- This dev host's kernel has `CONFIG_LEGACY_VSYSCALL_NONE=y`. glibc 2.11's `time()` (and
  so its `getaddrinfo`) jumps into the vsyscall page and segfaults inside a
  `debian/eol:squeeze` container. GitHub's Ubuntu runner kernels emulate vsyscall
  execution, so the squeeze smoke runs in CI; locally it can only be partly reproduced.
- io_uring needs Linux 5.1 and gets `EPERM` under docker's default seccomp; QEMU and
  anyfs already treat init failure as "unavailable".

---

## File structure

Created:

| file | responsibility |
|---|---|
| `scripts/lib/zig-cc.sh` | wrapper body: target, flag fixes, sccache hand-off |
| `scripts/lib/zig-cc`, `scripts/lib/zig-c++` | two-line launchers (build systems need a single executable path) |
| `scripts/fetch_zig.sh` | install the pinned zig where config expects it |
| `scripts/check_linux_abi.sh` | glibc floor / NEEDED / unversioned-undefined gate |
| `scripts/lib/sysroot_sources.sh` | pinned source URLs + sha256 shared by both sysroot scripts, plus `fetch`/`unpack` |
| `scripts/build_linux_sysroot.sh` | static dependency sysroot for linux-amd64 |
| `scripts/lib/linux_sysroot.manifest` | expected archives in that sysroot |
| `patches/sysroot/libaio-0.3.113-static-symver.patch` | libaio `.symver` removal |
| `patches/sysroot/fuse-3.18.3-posix-memalign.patch` | libfuse `aligned_alloc` removal |
| `scripts/lib/lkl-linux-cc.sh` | LKL CC dispatcher: kernel sub-make → gcc, tools/lkl → zig |
| `src/core/anyfs_tls.c`, `src/core/anyfs_tls.h` | point OpenSSL at the host CA bundle |
| `tests/test_zig_cc.sh`, `tests/test_check_linux_abi.sh`, `tests/test_lkl_linux_cc.sh`, `tests/unit/test_tls_ca.c` | gates |
| `ts/packages/anyfs-native/exports.map` | addon export list |

Modified: `build.config.toml`, `scripts/lib/config.sh`, `scripts/doctor.sh`,
`scripts/build_wasm_sysroot.sh`, `scripts/gen_lkl_config.sh`, `scripts/build_lkl.sh`,
`scripts/build_qemu.sh`, `scripts/build_anyfs.sh`, `scripts/build_anyfs_wasm.sh`,
`scripts/package_linux.sh`, `scripts/lint-shellcheck.sh`,
`scripts/lint-no-hardcoded-paths.sh`, `meson.build`, `src/core/qemu_thread.c`,
`src/bench/shmem_relay_bench.c`, `ts/packages/anyfs-native/binding.gyp`,
`ts/packages/anyfs-native/scripts/build-linux-electron.sh`,
`.github/workflows/linux.yml`, `docs/distribution.md`, the spec.

Outside this repo: `~/drivelist-anyfs/scripts/build-linux-electron.sh`,
`~/drivelist-anyfs/binding.gyp`, new `~/drivelist-anyfs/exports.map`. That checkout's
`anyfs/main` keeps all its anyfs changes uncommitted; leave these uncommitted too and
report them.

Generated, gitignored (`.toolchain/`): `.toolchain/zig` (symlink to the zig install),
`.toolchain/meson-native-linux-amd64.ini`.

---

## Task 1: Pin and install zig

**Files:**
- Modify: `build.config.toml`
- Modify: `scripts/lib/config.sh`
- Create: `scripts/fetch_zig.sh`
- Modify: `scripts/doctor.sh`

- [ ] **Step 1: Add the pins to `build.config.toml`**

In `[paths]`, after `wasm_sysroot`:

```toml
# Static dependency sysroot for linux-amd64 (zig cc, glibc 2.11 floor), built by
# scripts/build_linux_sysroot.sh.
# "" => ${XDG_CACHE_HOME:-~/.cache}/anyfs-linux-sysroot/x86_64-linux-gnu.2.11
linux_sysroot = ""
```

In `[toolchains]`, after `wasm_ld`:

```toml
# zig builds every linux-amd64 artifact (scripts/lib/zig-cc.sh). "" =>
# $HOME/zig-<zig_version>/zig, where scripts/fetch_zig.sh installs it.
zig         = ""
zig_version = "0.16.0"
zig_sha256  = "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"
```

- [ ] **Step 2: Resolve the defaults in `scripts/lib/config.sh`**

Insert before the final `export` line of `anyfs_load_config`:

```bash
    : "${ANYFS_PATHS_LINUX_SYSROOT:=${XDG_CACHE_HOME:-$HOME/.cache}/anyfs-linux-sysroot/x86_64-linux-gnu.2.11}"
    : "${ANYFS_TOOLCHAINS_ZIG:=$HOME/zig-$ANYFS_TOOLCHAINS_ZIG_VERSION/zig}"
    # scripts/lib/zig-cc.sh finds zig through this link, so build trees these
    # scripts configured keep working when ninja or `meson test` runs them
    # without our environment.
    if [[ -x "$ANYFS_TOOLCHAINS_ZIG" ]]; then
        local zdir
        zdir="$(cd "$(dirname "$ANYFS_TOOLCHAINS_ZIG")" && pwd -P)"
        if [[ "$(readlink "$root/.toolchain/zig" 2>/dev/null)" != "$zdir" ]]; then
            mkdir -p "$root/.toolchain"
            ln -sfn "$zdir" "$root/.toolchain/zig"
        fi
    fi
```

and extend the `export` line with `ANYFS_PATHS_LINUX_SYSROOT ANYFS_TOOLCHAINS_ZIG`.

- [ ] **Step 3: Write `scripts/fetch_zig.sh`**

```bash
#!/usr/bin/env bash
# Install the pinned zig (toolchains.zig_version in build.config.toml) where
# scripts/lib/config.sh expects it: toolchains.zig, default
# $HOME/zig-<version>/zig. Idempotent; fails if a different zig is already there.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

ver="$ANYFS_TOOLCHAINS_ZIG_VERSION"
zig="$ANYFS_TOOLCHAINS_ZIG"
dest="$(dirname "$zig")"

if [[ -x "$zig" ]]; then
    have="$("$zig" version)"
    if [[ "$have" != "$ver" ]]; then
        echo "Error: $zig is zig $have, build.config.toml pins $ver" >&2
        exit 1
    fi
    echo "zig $ver already installed at $dest"
    exit 0
fi
if [[ "$(basename "$zig")" != zig || -e "$dest" ]]; then
    echo "Error: won't install into $dest (toolchains.zig = $zig)" >&2
    exit 1
fi

mkdir -p "$(dirname "$dest")"
tmp="$(mktemp -d "$(dirname "$dest")/.zig-fetch.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
url="https://ziglang.org/download/$ver/zig-x86_64-linux-$ver.tar.xz"
echo ">>> fetch $url"
curl -fL --retry 3 -o "$tmp/zig.tar.xz" "$url"
echo "$ANYFS_TOOLCHAINS_ZIG_SHA256  $tmp/zig.tar.xz" | sha256sum --check --quiet -
tar -xf "$tmp/zig.tar.xz" -C "$tmp"
mv "$tmp/zig-x86_64-linux-$ver" "$dest"
echo "zig $("$dest/zig" version) installed at $dest"
```

`chmod +x scripts/fetch_zig.sh`.

- [ ] **Step 4: Run it and check the link**

Run: `./scripts/fetch_zig.sh && ls -l .toolchain/zig && .toolchain/zig/zig version`
Expected: `zig 0.16.0 installed at /home/kosaka/zig-0.16.0`, the link pointing there,
`0.16.0`. A second run prints `already installed`.

- [ ] **Step 5: Add the doctor check**

In `scripts/doctor.sh`, after the native binutils block:

```bash
echo "== zig (linux-amd64 builds; scripts/fetch_zig.sh) =="
if [ -x "$ANYFS_TOOLCHAINS_ZIG" ]; then
    zv="$("$ANYFS_TOOLCHAINS_ZIG" version 2>/dev/null)"
    [ "$zv" = "$ANYFS_TOOLCHAINS_ZIG_VERSION" ] \
        && ok "zig $zv ($ANYFS_TOOLCHAINS_ZIG)" \
        || bad "zig is '$zv', build.config.toml pins $ANYFS_TOOLCHAINS_ZIG_VERSION ($ANYFS_TOOLCHAINS_ZIG)"
else
    bad "zig missing at $ANYFS_TOOLCHAINS_ZIG — run scripts/fetch_zig.sh"
fi
```

Run: `./scripts/doctor.sh 2>&1 | grep -A1 '== zig'`
Expected: `ok   zig 0.16.0 (/home/kosaka/zig-0.16.0/zig)`.

- [ ] **Step 6: Lint lists**

Add `scripts/fetch_zig.sh` to the `checked` list in `scripts/lint-shellcheck.sh` and to
`migrated` in `scripts/lint-no-hardcoded-paths.sh`.
Run: `./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh`
Expected: both pass.

- [ ] **Step 7: Commit**

```bash
git add build.config.toml scripts/lib/config.sh scripts/fetch_zig.sh scripts/doctor.sh \
        scripts/lint-shellcheck.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "build(linux): pin zig 0.16.0 and install it with fetch_zig.sh"
```

---

## Task 2: ABI gate `scripts/check_linux_abi.sh`

**Files:**
- Create: `scripts/check_linux_abi.sh`
- Create: `tests/test_check_linux_abi.sh`
- Modify: `meson.build` (register the test)

- [ ] **Step 1: Write the failing test `tests/test_check_linux_abi.sh`**

```bash
#!/usr/bin/env bash
# Gate for scripts/check_linux_abi.sh, on fixtures built with the pinned zig:
# a binary within the floor passes; one needing a newer glibc, one with a
# non-glibc NEEDED entry, and a shared object with an unversioned undefined
# symbol fail. Skips (77) when zig isn't installed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../scripts/lib/config.sh
source "$root/scripts/lib/config.sh"
zig="$root/.toolchain/zig/zig"
[[ -x "$zig" ]] || { echo "SKIP: zig not installed (scripts/fetch_zig.sh)"; exit 77; }
check="$root/scripts/check_linux_abi.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cc() { local t="$1"; shift; "$zig" cc -target "x86_64-linux-gnu.$t" -g0 "$@"; }
pass() { "$check" "$@" >"$tmp/out" 2>&1 || { cat "$tmp/out"; echo "FAIL: expected pass: $*"; exit 1; }; }
deny() { if "$check" "$@" >"$tmp/out" 2>&1; then cat "$tmp/out"; echo "FAIL: expected failure: $*"; exit 1; fi; }

printf '#include <stdio.h>\nint main(void){puts("x");return 0;}\n' > "$tmp/old.c"
cc 2.11 "$tmp/old.c" -o "$tmp/old"
printf '#include <sys/random.h>\nint main(void){char b[4];return getrandom(b,4,0)<0;}\n' > "$tmp/new.c"
cc 2.25 "$tmp/new.c" -o "$tmp/new"
printf 'int foo(void){return 1;}\n' > "$tmp/foo.c"
cc 2.11 -shared -fPIC "$tmp/foo.c" -o "$tmp/libfoo.so"
printf 'int foo(void);\nint main(void){return foo();}\n' > "$tmp/usefoo.c"
cc 2.11 "$tmp/usefoo.c" -L"$tmp" -lfoo -o "$tmp/usefoo"
printf '#define _GNU_SOURCE\n#include <sys/mman.h>\nint f(void){return memfd_create("x",0);}\n' > "$tmp/so.c"
cc 2.11 -shared -fPIC "$tmp/so.c" -o "$tmp/libundef.so"

pass 2.11 "$tmp/old"
deny 2.11 "$tmp/new"
pass 2.25 "$tmp/new"
deny 2.11 "$tmp/usefoo"
deny 2.11 "$tmp/libundef.so"
pass --allow-undefined='^memfd_create$' 2.11 "$tmp/libundef.so"
# Directories recurse; non-ELF files are skipped, ELF files still count.
mkdir "$tmp/tree"; cp "$tmp/old" "$tmp/tree/"; echo text > "$tmp/tree/README"
pass 2.11 "$tmp/tree"
cp "$tmp/new" "$tmp/tree/"
deny 2.11 "$tmp/tree"

echo "OK: check_linux_abi.sh enforces floor, NEEDED allowlist and versioned imports"
```

`chmod +x tests/test_check_linux_abi.sh`.

- [ ] **Step 2: Run it to see it fail**

Run: `tests/test_check_linux_abi.sh`
Expected: FAIL (`check_linux_abi.sh: No such file or directory` on the first `pass`).

- [ ] **Step 3: Write `scripts/check_linux_abi.sh`**

```bash
#!/usr/bin/env bash
# Gate for linux-amd64 artifacts. Fails if an ELF file needs a newer glibc
# than the floor, links a shared library outside the allowlist, or carries an
# undefined dynamic symbol with no version.
#
# Usage: check_linux_abi.sh [--allow-undefined=ERE] <max-glibc> <file|dir>...
#   <max-glibc>  2.11 for the CLI tarball, 2.25 for code loaded into Electron.
#   --allow-undefined=ERE
#                unversioned undefined symbols matching ERE are expected, e.g.
#                an addon's napi_* imports, which the host process provides.
#   Directories are searched recursively; files that aren't ELF are skipped.
#
# Why the third check: a function the floor's glibc lacks fails an executable
# link, but a -shared link silently leaves it undefined with no version. The
# version scan never sees it, and the library only fails to load on an older
# system. Needs binutils >= 2.35 (nm -D prints symbol versions).
set -euo pipefail

# glibc's own libraries, plus the two the tarball bundles.
ALLOWED_NEEDED=(
    libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2
    ld-linux-x86-64.so.2
    liblkl.so libanyfs-qemublk.so
)

usage() {
    awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0" >&2
    exit 2
}

allow_undef=""
case "${1:-}" in
    --allow-undefined=*) allow_undef="${1#*=}"; shift ;;
esac
[[ $# -ge 2 ]] || usage
max="$1"
shift

# ver_gt A B: version A is newer than B.
ver_gt() { [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" == "$1" ]]; }

is_elf() { [[ "$(head -c4 "$1" | od -An -tx1 | tr -d ' \n')" == 7f454c46 ]]; }

fail=0
checked=0

check_file() {
    local f="$1" top="" v lib sym ok
    local -a bad=()
    checked=$((checked + 1))

    # Highest GLIBC_x.y in the version-needs section (.gnu.version_r).
    while read -r v; do
        if [[ "$v" == PRIVATE ]]; then
            bad+=("uses GLIBC_PRIVATE")
            continue
        fi
        if [[ -z "$top" ]] || ver_gt "$v" "$top"; then top="$v"; fi
    done < <(readelf -V --wide "$f" | sed -n '/Version needs section/,$p' \
                 | grep -oE 'Name: GLIBC_[0-9A-Z_.]+' | sed 's/^Name: GLIBC_//')
    if [[ -n "$top" ]] && ver_gt "$top" "$max"; then
        bad+=("needs GLIBC_$top > $max:$(objdump -T "$f" | grep -oE "GLIBC_$top +[^ ]+" | sed 's/^GLIBC_[^ ]* */ /' | sort -u | tr -d '\n')")
    fi

    while read -r lib; do
        ok=0
        for v in "${ALLOWED_NEEDED[@]}"; do [[ "$lib" == "$v" ]] && ok=1; done
        [[ $ok -eq 1 ]] || bad+=("NEEDED $lib is not glibc or bundled")
    done < <(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')

    while read -r sym; do
        [[ -n "$allow_undef" && "$sym" =~ $allow_undef ]] && continue
        bad+=("undefined symbol $sym has no version (missing at the floor?)")
    done < <(nm -D --undefined-only "$f" | awk '$1 == "U" && $2 !~ /@/ {print $2}')

    if [[ ${#bad[@]} -gt 0 ]]; then
        printf 'FAIL %s\n' "$f"
        printf '       %s\n' "${bad[@]}"
        fail=1
    else
        printf 'ok   %s (GLIBC_%s)\n' "$f" "${top:-none}"
    fi
}

for arg in "$@"; do
    while IFS= read -r -d '' f; do
        is_elf "$f" && check_file "$f"
    done < <(find "$arg" -type f -print0)
done

if [[ $checked -eq 0 ]]; then
    echo "check_linux_abi: no ELF files in: $*" >&2
    exit 1
fi
[[ $fail -eq 0 ]] && echo "check_linux_abi: $checked ELF file(s) within GLIBC_$max"
exit "$fail"
```

`chmod +x scripts/check_linux_abi.sh`.

- [ ] **Step 4: Run the test**

Run: `tests/test_check_linux_abi.sh`
Expected: `OK: check_linux_abi.sh enforces floor, NEEDED allowlist and versioned imports`.

- [ ] **Step 5: Register the test and lint**

In `meson.build`, next to `test('lkl_mingw_cc', …)`:

```meson
    # Build-script gates for the zig/glibc-floor toolchain (skip without zig).
    test('check_linux_abi', find_program('tests/test_check_linux_abi.sh'), suite: 'unit')
```

Add `scripts/check_linux_abi.sh` to both lint lists.
Run: `./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh`

- [ ] **Step 6: Commit**

```bash
git add scripts/check_linux_abi.sh tests/test_check_linux_abi.sh meson.build \
        scripts/lint-shellcheck.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "build(linux): add check_linux_abi.sh, the glibc-floor and NEEDED gate"
```

---

## Task 3: zig-cc wrapper

**Files:**
- Create: `scripts/lib/zig-cc.sh`, `scripts/lib/zig-cc`, `scripts/lib/zig-c++`
- Create: `tests/test_zig_cc.sh`
- Modify: `meson.build`, lint lists

- [ ] **Step 1: Write the failing test `tests/test_zig_cc.sh`**

```bash
#!/usr/bin/env bash
# Gate for scripts/lib/zig-cc.sh: GNU-style -dumpmachine, no host library
# dirs, none of zig's NDEBUG/UBSan/DWARF defaults (an explicit flag still
# wins), baseline x86-64 predefines and a glibc floor that holds for both
# targets. Skips (77) when the pinned zig isn't installed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../scripts/lib/config.sh
source "$root/scripts/lib/config.sh"
[[ -x "$root/.toolchain/zig/zig" ]] || { echo "SKIP: zig not installed (scripts/fetch_zig.sh)"; exit 77; }
cc="$root/scripts/lib/zig-cc"
cxx="$root/scripts/lib/zig-c++"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

[[ "$("$cc" -dumpmachine)" == x86_64-unknown-linux-gnu ]] || fail "-dumpmachine"
[[ "$(ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.25 "$cc" -dumpmachine)" == x86_64-unknown-linux-gnu ]] \
    || fail "-dumpmachine at 2.25"
if "$cc" -print-search-dirs | grep -q /; then fail "-print-search-dirs lists directories"; fi

printf '#include <assert.h>\nint f(int x){assert(x);return x+1;}\n' > "$tmp/f.c"
"$cc" -O2 -c "$tmp/f.c" -o "$tmp/f.o"
if readelf -S "$tmp/f.o" | grep -q debug_info; then fail "DWARF without -g"; fi
nm "$tmp/f.o" | grep -q __assert_fail || fail "assert() compiled out at -O2"
"$cc" -O2 -g -c "$tmp/f.c" -o "$tmp/g.o"
readelf -S "$tmp/g.o" | grep -q debug_info || fail "explicit -g ignored"
"$cc" -O0 -c "$tmp/f.c" -o "$tmp/u.o"
if nm "$tmp/u.o" | grep -q __ubsan; then fail "UBSan at -O0"; fi
if ! "$cc" -O2 -DNDEBUG -dM -E -x c /dev/null | grep -q '#define NDEBUG'; then
    fail "explicit -DNDEBUG ignored"
fi

printf '#include <stdio.h>\n#include <time.h>\nint main(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);puts("ok");return 0;}\n' > "$tmp/h.c"
printf '#include <cstdio>\n#include <string>\nint main(){std::string s("ok");std::puts(s.c_str());return 0;}\n' > "$tmp/h.cc"
for floor in 2.11 2.25; do
    export ANYFS_ZIG_TARGET="x86_64-linux-gnu.$floor"
    defs="$("$cc" -O2 -dM -E -x c /dev/null)"
    for m in __AVX__ __AVX2__ __SSE4_2__ __SSE3__ NDEBUG; do
        if grep -q "#define $m " <<<"$defs"; then fail "$floor predefines $m"; fi
    done
    grep -q "#define __GLIBC_MINOR__ ${floor#2.}$" <<<"$defs" || fail "$floor: __GLIBC_MINOR__"
    "$cc" -O2 "$tmp/h.c" -o "$tmp/h"
    [[ "$("$tmp/h")" == ok ]] || fail "$floor: C hello"
    "$root/scripts/check_linux_abi.sh" "$floor" "$tmp/h" >/dev/null || fail "$floor: C floor"
    if [[ $floor == 2.25 ]]; then
        "$cxx" -O2 "$tmp/h.cc" -o "$tmp/hx"
        [[ "$("$tmp/hx")" == ok ]] || fail "C++ hello"
        "$root/scripts/check_linux_abi.sh" 2.25 "$tmp/hx" >/dev/null || fail "C++ floor/NEEDED"
    fi
done
unset ANYFS_ZIG_TARGET

echo "OK: zig-cc.sh gives gcc-like defaults at the pinned floors"
```

`chmod +x tests/test_zig_cc.sh`. Run it: expected FAIL (wrapper missing).

- [ ] **Step 2: Write `scripts/lib/zig-cc.sh`**

```sh
#!/bin/sh
# scripts/lib/zig-cc.sh — body of the zig-cc / zig-c++ compiler wrappers that
# build every linux-amd64 artifact. Build systems get the two launchers next
# to this file (scripts/lib/zig-cc, scripts/lib/zig-c++), never this script.
#
# Environment:
#   ANYFS_ZIG_TARGET   zig target; default x86_64-linux-gnu.2.11, the glibc
#                      floor of the CLI tools and libraries. Code loaded into
#                      Electron uses x86_64-linux-gnu.2.25. An explicit target
#                      also pins the baseline x86-64 CPU, whatever the host.
#   ANYFS_ZIG          zig binary; default <repo>/.toolchain/zig/zig, the link
#                      scripts/lib/config.sh keeps pointed at toolchains.zig.
#   ANYFS_ZIG_SCCACHE  1 = compile through `sccache <zig> cc|c++ …`. The
#                      sccache fork recognises zig only by an executable stem
#                      of `zig` with argv1 cc/c++, so it must get zig itself,
#                      never this wrapper. The target stays a visible
#                      argument, so the cache is partitioned by target.
#
# Where zig cc differs from the gcc that build systems expect:
#   -dumpmachine        zig prints its versioned target and then rejects it;
#                       meson and autoconf need a plain GNU triple.
#   -print-search-dirs  zig reports the HOST gcc's library dirs, and meson's
#                       find_library() then links /usr/lib/x86_64-linux-gnu
#                       libraries. Report none: libraries come from -L and
#                       pkg-config only.
#   -UNDEBUG            zig defines NDEBUG at -O1 and above.
#   -fno-sanitize=undefined  zig enables UBSan at -O0.
#   -g0                 zig emits DWARF even without -g.
# The last three go before the caller's flags, so an explicit -DNDEBUG,
# -fsanitize or -g still wins.
mode=$1
shift
target=${ANYFS_ZIG_TARGET:-x86_64-linux-gnu.2.11}
zig=${ANYFS_ZIG:-$(cd "$(dirname "$0")/../.." && pwd)/.toolchain/zig/zig}

for a in "$@"; do
    case $a in
    -dumpmachine)
        case $target in
        *-linux-gnu*) echo "${target%%-*}-unknown-linux-gnu" ;;
        *) echo "${target%%.*}" ;;
        esac
        exit 0
        ;;
    -print-search-dirs | --print-search-dirs)
        printf 'install: \nprograms: =\nlibraries: =\n'
        exit 0
        ;;
    esac
done

if [ "${ANYFS_ZIG_SCCACHE:-0}" = 1 ]; then
    exec sccache "$zig" "$mode" -target "$target" \
        -UNDEBUG -fno-sanitize=undefined -g0 "$@"
fi
exec "$zig" "$mode" -target "$target" -UNDEBUG -fno-sanitize=undefined -g0 "$@"
```

- [ ] **Step 3: Write the launchers**

`scripts/lib/zig-cc`:

```sh
#!/bin/sh
# zig cc for linux-amd64 builds; see zig-cc.sh.
exec "$(dirname "$0")/zig-cc.sh" cc "$@"
```

`scripts/lib/zig-c++`:

```sh
#!/bin/sh
# zig c++ for linux-amd64 builds; see zig-cc.sh.
exec "$(dirname "$0")/zig-cc.sh" c++ "$@"
```

`chmod +x scripts/lib/zig-cc.sh scripts/lib/zig-cc scripts/lib/zig-c++`.

- [ ] **Step 4: Run the test**

Run: `tests/test_zig_cc.sh`
Expected: `OK: zig-cc.sh gives gcc-like defaults at the pinned floors`.

- [ ] **Step 5: Register, lint, commit**

`meson.build`, next to the Task 2 test:

```meson
    test('zig_cc', find_program('tests/test_zig_cc.sh'), suite: 'unit')
```

Add `scripts/lib/zig-cc.sh scripts/lib/zig-cc scripts/lib/zig-c++` to the shellcheck list
and to `migrated`.

```bash
./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh
git add scripts/lib/zig-cc.sh scripts/lib/zig-cc scripts/lib/zig-c++ tests/test_zig_cc.sh \
        meson.build scripts/lint-shellcheck.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "build(linux): add the zig-cc wrapper with gcc-like defaults"
```

---

## Task 4: Share the source pins between the two sysroots

**Files:**
- Create: `scripts/lib/sysroot_sources.sh`
- Modify: `scripts/build_wasm_sysroot.sh`

- [ ] **Step 1: Write `scripts/lib/sysroot_sources.sh`**

Move `fetch()`, `unpack()` and every `*_V`/`*_URL`/`*_SHA` assignment out of
`build_wasm_sysroot.sh` into this file unchanged, and add the linux-only pins:

```bash
# shellcheck shell=bash
# scripts/lib/sysroot_sources.sh — pinned upstream sources for
# scripts/build_wasm_sysroot.sh and scripts/build_linux_sysroot.sh, so a
# library both sysroots carry has one version. Also the download helpers both
# use. Callers set WORK (download + unpack dir) before calling fetch/unpack.

# fetch <url> <sha256> <dest> — download (with cache) and verify.
fetch() {
    local url="$1" sha="$2" dest="$3"
    if [[ ! -f "$dest" ]] || ! echo "$sha  $dest" | sha256sum --check --quiet - 2>/dev/null; then
        echo ">>> fetch $url"
        curl -fL --retry 3 -o "$dest" "$url"
    fi
    echo "$sha  $dest" | sha256sum --check --quiet -
}

# unpack <tarball> <dirname> — fresh-extract into $WORK/<dirname>.
unpack() {
    local tarball="$1" dirname="$2"
    rm -rf "${WORK:?}/$dirname"
    tar -xf "$tarball" -C "$WORK"
    [[ -d "$WORK/$dirname" ]] || { echo "expected $dirname after extracting $tarball" >&2; exit 1; }
}

# ── both sysroots ────────────────────────────────────────────────────────
ZLIB_V=1.3.1
# zlib.net 404s superseded releases; fossils/ archives every version.
ZLIB_URL="https://zlib.net/fossils/zlib-$ZLIB_V.tar.gz"
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
BZ2_V=1.0.8
BZ2_URL="https://sourceware.org/pub/bzip2/bzip2-$BZ2_V.tar.gz"
BZ2_SHA=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
ZSTD_V=1.5.7
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v$ZSTD_V/zstd-$ZSTD_V.tar.gz"
ZSTD_SHA=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
FFI_V=3.5.2
FFI_URL="https://github.com/libffi/libffi/releases/download/v$FFI_V/libffi-$FFI_V.tar.gz"
FFI_SHA=f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc
GLIB_V=2.88.0
GLIB_URL="https://download.gnome.org/sources/glib/${GLIB_V%.*}/glib-$GLIB_V.tar.xz"
GLIB_SHA=3546251ccbb3744d4bc4eb48354540e1f6200846572bab68e3a2b7b2b64dfd07
# util-linux: the wasm recipe builds the peru checkout (paths.util_linux); the
# linux recipe uses the release tarball, which ships a generated configure.
UL_V=2.40.4
UL_URL="https://www.kernel.org/pub/linux/utils/util-linux/v${UL_V%.*}/util-linux-$UL_V.tar.xz"
UL_SHA=5c1daf733b04e9859afdc3bd87cc481180ee0f88b5c0946b16fdec931975fb79

# ── linux sysroot only ───────────────────────────────────────────────────
AIO_V=0.3.113
AIO_URL="https://releases.pagure.org/libaio/libaio-$AIO_V.tar.gz"
AIO_SHA=2c44d1c5fd0d43752287c9ae1eb9c023f04ef848ea8d4aafa46e9aedb678200b
URING_V=2.15
URING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_V.tar.gz"
URING_SHA=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
FUSE_V=3.18.3
FUSE_URL="https://github.com/libfuse/libfuse/releases/download/fuse-$FUSE_V/fuse-$FUSE_V.tar.gz"
FUSE_SHA=bcd19582c5e30f7fe45dd86a5540e998590aa01903afc7ebcbeea6c8ac5421ee
OPENSSL_V=3.5.9
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_V/openssl-$OPENSSL_V.tar.gz"
OPENSSL_SHA=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
CURL_V=8.22.0
CURL_URL="https://curl.se/download/curl-$CURL_V.tar.xz"
CURL_SHA=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
```

- [ ] **Step 2: Source it from `build_wasm_sysroot.sh`**

Delete the moved `fetch`/`unpack` definitions and the `ZLIB_*`, `BZ2_*`, `ZSTD_*`,
`FFI_*`, `GLIB_*` assignments (keep each recipe's comment block), and after
`NPROC="$(nproc)"` add:

```bash
# shellcheck source=lib/sysroot_sources.sh
source "$SCRIPT_DIR/lib/sysroot_sources.sh"
```

`build_blkid` keeps using `UL_V` and `UL_SRC` (its own version check against the
checkout), so its `UL_V=2.40.4` line is deleted too.

- [ ] **Step 3: Verify the wasm script still builds a recipe**

Run: `SYSROOT=$HOME/.cache/anyfs-wasm-sysroot-check ./scripts/build_wasm_sysroot.sh --only=zlib && ls $HOME/.cache/anyfs-wasm-sysroot-check/lib/libz.a && rm -rf $HOME/.cache/anyfs-wasm-sysroot-check`
Expected: `=== zlib 1.3.1 ===`, the archive exists, `(partial build via --only=zlib …)`.
Then `./scripts/lint-shellcheck.sh`.

- [ ] **Step 4: Commit**

```bash
git add scripts/lib/sysroot_sources.sh scripts/build_wasm_sysroot.sh
git commit -m "build: share the sysroot source pins between wasm and linux"
```

---

## Task 5: `scripts/build_linux_sysroot.sh`

**Files:**
- Create: `patches/sysroot/libaio-0.3.113-static-symver.patch`
- Create: `patches/sysroot/fuse-3.18.3-posix-memalign.patch`
- Create: `scripts/build_linux_sysroot.sh`
- Create: `scripts/lib/linux_sysroot.manifest`
- Modify: lint lists

- [ ] **Step 1: Generate the two source patches**

```bash
w=$(mktemp -d ~/.cache/sysroot-patch.XXXXXX)
source scripts/lib/sysroot_sources.sh; WORK=$w
fetch "$AIO_URL" "$AIO_SHA" "$w/aio.tar.gz"; tar -xf "$w/aio.tar.gz" -C "$w"
cp -a "$w/libaio-$AIO_V" "$w/a"
sed -i \
  -e '/^#define SYMVER(compat_sym, orig_sym, ver_sym)/{n;s/.*/\t\/* static archive: no versioned compat symbols *\//}' \
  -e '/^#define DEFSYMVER(compat_sym, orig_sym, ver_sym)/{n;s/.*/\textern __typeof(compat_sym) orig_sym __attribute__((alias(SYMSTR(compat_sym))));/}' \
  "$w/libaio-$AIO_V/src/syscall.h"
mkdir -p patches/sysroot
{ cat <<'EOF'
libaio: drop symbol versions for a static, -fPIC archive

src/syscall.h tags io_getevents, io_cancel and io_queue_wait with .symver
(@@LIBAIO_0.4 plus @LIBAIO_0.1 compat aliases). In a static archive linked
into a shared object (libanyfs-qemublk.so) those tags fail the link with
"symbol io_getevents@@LIBAIO_0.4 has undefined version LIBAIO_0.4" (lld and
GNU ld alike). Make the default version a plain alias and drop the compat
symbols; versions mean nothing inside an archive.

EOF
  cd "$w" && diff -u a/src/syscall.h "libaio-$AIO_V/src/syscall.h" | sed "s|^+++ libaio-$AIO_V/|+++ b/|"; } \
  > patches/sysroot/libaio-0.3.113-static-symver.patch || true

fetch "$FUSE_URL" "$FUSE_SHA" "$w/fuse.tar.gz"; tar -xf "$w/fuse.tar.gz" -C "$w"
cp -a "$w/fuse-$FUSE_V" "$w/f"
sed -i 's|^\(\t*\)char \*buf = aligned_alloc(pagesize, new_size);|\1void *raw = NULL;\n\1char *buf = posix_memalign(\&raw, pagesize, new_size) ? NULL : raw;|' \
  "$w/fuse-$FUSE_V/lib/fuse_lowlevel.c"
{ cat <<'EOF'
libfuse: allocate the page-aligned buffer with posix_memalign

aligned_alloc() is glibc 2.16; the linux-amd64 build targets glibc 2.11.
The call allocates a page-multiple size at page alignment, which
posix_memalign does identically (already used twice in this file), and the
buffer is released with free() either way.

EOF
  cd "$w" && diff -u f/lib/fuse_lowlevel.c "fuse-$FUSE_V/lib/fuse_lowlevel.c" | sed "s|^+++ fuse-$FUSE_V/|+++ b/|;s|^--- f/|--- a/|"; } \
  > patches/sysroot/fuse-3.18.3-posix-memalign.patch || true
rm -rf "$w"
```

Then check both patches: the `---`/`+++` lines read `a/<path>` / `b/<path>`, and
`grep -c '^[-+][^-+]' patches/sysroot/*.patch` shows 2–4 changed lines each. Fix the
headers by hand if `diff` printed the work-dir prefix.

- [ ] **Step 2: Write `scripts/build_linux_sysroot.sh`**

```bash
#!/usr/bin/env bash
# Static dependency sysroot for the linux-amd64 build.
#
# Builds every third-party library the linux-amd64 artifacts link as a static,
# -fPIC archive with scripts/lib/zig-cc (target x86_64-linux-gnu.2.11: the
# glibc floor, baseline x86-64), and installs headers, archives and .pc files
# into $SYSROOT (paths.linux_sysroot in build.config.toml). Consumers build
# with PKG_CONFIG_LIBDIR pointing only at $SYSROOT/lib/pkgconfig, so a host
# library can never leak in. Versions come from scripts/lib/sysroot_sources.sh
# (shared with the wasm sysroot). The Electron addons link these same
# archives at their 2.25 floor: objects carry no symbol versions, the final
# link picks them.
#
# Usage:
#   ./scripts/build_linux_sysroot.sh                 # everything
#   ./scripts/build_linux_sysroot.sh --only=curl     # one recipe (needs its deps)
#   ./scripts/build_linux_sysroot.sh --clean         # wipe $SYSROOT and the work dir first
#
# Env overrides: SYSROOT, WORK, JOBS.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

SYSROOT="${SYSROOT:-$ANYFS_PATHS_LINUX_SYSROOT}"
WORK="${WORK:-$REPO_ROOT/build-linux-sysroot}"
JOBS="${JOBS:-$(nproc)}"
PATCHES="$REPO_ROOT/patches/sysroot"

ONLY=""
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --only=*) ONLY="${arg#--only=}" ;;
        --clean)  CLEAN=1 ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0 ;;
        *) echo "unknown argument: $arg (try --only=<lib>, --clean)" >&2; exit 1 ;;
    esac
done

for tool in meson ninja pkg-config perl make curl python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool not on PATH" >&2; exit 1; }
done
[[ -x "$REPO_ROOT/.toolchain/zig/zig" ]] || { echo "zig not installed: run scripts/fetch_zig.sh" >&2; exit 1; }

[[ $CLEAN -eq 1 ]] && rm -rf "$WORK" "$SYSROOT"
mkdir -p "$WORK" "$SYSROOT/lib/pkgconfig" "$SYSROOT/include"

# shellcheck source=lib/sysroot_sources.sh
source "$SCRIPT_DIR/lib/sysroot_sources.sh"

export CC="$SCRIPT_DIR/lib/zig-cc" CXX="$SCRIPT_DIR/lib/zig-c++"
export ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.11
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
CFLAGS_BASE="-O2 -fPIC"

# Meson native file for every meson recipe: the wrappers, the sysroot as the
# only pkg-config root, and no cmake (its dependency fallback finds host
# packages through /usr/bin/cmake).
cat > "$WORK/native.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
cmake = 'false'

[properties]
pkg_config_libdir = ['$SYSROOT/lib/pkgconfig']
EOF

apply_patch() {  # <dir> <patch>
    patch -d "$1" -p1 --forward --silent < "$PATCHES/$2"
}

# ---------------------------------------------------------------------------
build_zlib() {
    echo "=== zlib $ZLIB_V ==="
    fetch "$ZLIB_URL" "$ZLIB_SHA" "$WORK/zlib-$ZLIB_V.tar.gz"
    unpack "$WORK/zlib-$ZLIB_V.tar.gz" "zlib-$ZLIB_V"
    cd "$WORK/zlib-$ZLIB_V"
    CFLAGS="$CFLAGS_BASE" ./configure --prefix="$SYSROOT" --static
    make -j"$JOBS" libz.a
    make install
}

# bzip2 has no build system worth driving (hard-coded cc tests) and no .pc;
# compile the 7 library sources directly, like the wasm recipe.
build_bzip2() {
    echo "=== bzip2 $BZ2_V ==="
    fetch "$BZ2_URL" "$BZ2_SHA" "$WORK/bzip2-$BZ2_V.tar.gz"
    unpack "$WORK/bzip2-$BZ2_V.tar.gz" "bzip2-$BZ2_V"
    cd "$WORK/bzip2-$BZ2_V"
    local s objs=()
    for s in blocksort bzlib compress crctable decompress huffman randtable; do
        # shellcheck disable=SC2086
        "$CC" $CFLAGS_BASE -D_FILE_OFFSET_BITS=64 -c "$s.c" -o "$s.o"
        objs+=("$s.o")
    done
    rm -f libbz2.a
    ar rcs libbz2.a "${objs[@]}"
    install -m644 libbz2.a "$SYSROOT/lib/libbz2.a"
    install -m644 bzlib.h "$SYSROOT/include/bzlib.h"
}

build_zstd() {
    echo "=== zstd $ZSTD_V ==="
    fetch "$ZSTD_URL" "$ZSTD_SHA" "$WORK/zstd-$ZSTD_V.tar.gz"
    unpack "$WORK/zstd-$ZSTD_V.tar.gz" "zstd-$ZSTD_V"
    cd "$WORK/zstd-$ZSTD_V"
    CFLAGS="$CFLAGS_BASE" make -C lib -j"$JOBS" libzstd.a
    CFLAGS="$CFLAGS_BASE" make -C lib PREFIX="$SYSROOT" \
        install-static install-includes install-pc
}

build_libffi() {
    echo "=== libffi $FFI_V ==="
    fetch "$FFI_URL" "$FFI_SHA" "$WORK/libffi-$FFI_V.tar.gz"
    unpack "$WORK/libffi-$FFI_V.tar.gz" "libffi-$FFI_V"
    cd "$WORK/libffi-$FFI_V"
    CFLAGS="$CFLAGS_BASE" ./configure --host=x86_64-linux-gnu \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --disable-dependency-tracking --disable-multi-os-directory --disable-docs
    make -j"$JOBS"
    make install
}

# glib + pcre2 (forced meson subproject; the wrap file pins and verifies it).
build_glib() {
    echo "=== glib $GLIB_V (+pcre2 subproject) ==="
    fetch "$GLIB_URL" "$GLIB_SHA" "$WORK/glib-$GLIB_V.tar.xz"
    unpack "$WORK/glib-$GLIB_V.tar.xz" "glib-$GLIB_V"
    cd "$WORK/glib-$GLIB_V"
    meson setup _build --native-file "$WORK/native.ini" \
        -Dprefix="$SYSROOT" -Dlibdir=lib \
        -Dbuildtype=release \
        -Ddefault_library=static -Db_staticpic=true \
        -Dforce_fallback_for=pcre2 \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dlibelf=disabled \
        -Dsysprof=disabled -Dintrospection=disabled \
        -Dnls=disabled -Dtests=false -Dman-pages=disabled -Ddocumentation=false \
        -Dglib_debug=disabled
    # glib probes pthread_getname_np with has_header_symbol; zig's headers
    # declare it, but the symbol is glibc 2.12, above the floor, so every glib
    # tool fails to link. Drop the define (config.h is written once at setup).
    # Assert it was there so a glib bump that renames it fails loudly.
    grep -q '#define HAVE_PTHREAD_GETNAME_NP 1' _build/config.h || {
        echo "HAVE_PTHREAD_GETNAME_NP not in _build/config.h — glib changed; revisit this edit" >&2
        exit 1
    }
    sed -i '/#define HAVE_PTHREAD_GETNAME_NP 1/d' _build/config.h
    meson compile -C _build -j "$JOBS"
    meson install -C _build --no-rebuild
}

# libblkid + libuuid from the release tarball (generated configure, so no
# autotools). -Dcrc32c renames util-linux's crc32c: QEMU's libqemuutil.a
# defines a different crc32c (XOR'd result) and both end up in one link —
# duplicate symbol, or VHDX/ext4 checksums silently wrong. __secure_getenv:
# the link probe finds the 2.2.5 compat symbol but current headers don't
# declare it (lib/env.c then fails); without it safe_getenv() still refuses
# setuid callers.
build_blkid() {
    echo "=== util-linux $UL_V (libblkid + libuuid) ==="
    fetch "$UL_URL" "$UL_SHA" "$WORK/util-linux-$UL_V.tar.xz"
    unpack "$WORK/util-linux-$UL_V.tar.xz" "util-linux-$UL_V"
    rm -rf "$WORK/util-linux-build"
    mkdir -p "$WORK/util-linux-build"
    cd "$WORK/util-linux-build"
    CFLAGS="$CFLAGS_BASE -Dcrc32c=anyfs_blkid_crc32c" \
    ac_cv_func___secure_getenv=no \
    "$WORK/util-linux-$UL_V/configure" \
        --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-gnu \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --enable-libblkid --enable-libuuid --disable-all-programs \
        --disable-nls --disable-asciidoc \
        --without-systemd --without-systemdsystemunitdir \
        --without-tinfo --without-readline --without-ncurses --without-ncursesw \
        --without-cap-ng --without-audit --without-libmagic \
        --without-econf --without-cryptsetup \
        --without-util --without-python --without-selinux --without-utempter
    make -j"$JOBS" libblkid.la libuuid.la libblkid/blkid.pc libuuid/uuid.pc
    install -m644 .libs/libblkid.a .libs/libuuid.a "$SYSROOT/lib/"
    mkdir -p "$SYSROOT/include/blkid" "$SYSROOT/include/uuid"
    install -m644 libblkid/src/blkid.h "$SYSROOT/include/blkid/blkid.h"
    install -m644 "$WORK/util-linux-$UL_V/libuuid/src/uuid.h" "$SYSROOT/include/uuid/uuid.h"
    install -m644 libblkid/blkid.pc libuuid/uuid.pc "$SYSROOT/lib/pkgconfig/"
}

# libaio: upstream ships no .pc; write one. CFLAGS must come through the
# environment — src/Makefile does `CFLAGS ?=` then `+= -I. -fPIC`, which a
# command-line CFLAGS would override.
build_libaio() {
    echo "=== libaio $AIO_V ==="
    fetch "$AIO_URL" "$AIO_SHA" "$WORK/libaio-$AIO_V.tar.gz"
    unpack "$WORK/libaio-$AIO_V.tar.gz" "libaio-$AIO_V"
    cd "$WORK/libaio-$AIO_V"
    apply_patch . libaio-0.3.113-static-symver.patch
    CFLAGS="-O2" ENABLE_SHARED=0 make -j"$JOBS"
    CFLAGS="-O2" ENABLE_SHARED=0 make install prefix="$SYSROOT"
    cat > "$SYSROOT/lib/pkgconfig/libaio.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libaio
Description: Linux-native asynchronous I/O access library
URL: https://pagure.io/libaio
Version: $AIO_V
Libs: -L\${libdir} -laio
Cflags: -I\${includedir}
EOF
}

# liburing: --use-libc, because nolibc mode adds -nostdlib to the compile
# flags and zig cc then drops the libc include paths. The archive's objects
# use plain CFLAGS, so -fPIC goes there.
build_liburing() {
    echo "=== liburing $URING_V ==="
    fetch "$URING_URL" "$URING_SHA" "$WORK/liburing-$URING_V.tar.gz"
    unpack "$WORK/liburing-$URING_V.tar.gz" "liburing-liburing-$URING_V"
    cd "$WORK/liburing-liburing-$URING_V"
    CFLAGS="$CFLAGS_BASE" ./configure --cc="$CC" --cxx="$CXX" --use-libc \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" --libdevdir="$SYSROOT/lib" \
        --includedir="$SYSROOT/include" --mandir="$WORK/liburing-man"
    CFLAGS="$CFLAGS_BASE" make -C src -j"$JOBS" ENABLE_SHARED=0
    CFLAGS="$CFLAGS_BASE" make install ENABLE_SHARED=0
}

# libfuse3: --bindir is the compiled-in fusermount3 location. glibc < 2.24
# never reaches libfuse's PATH fallback (posix_spawn reports success and the
# child exits 127), so it must be the real distro path; nothing is installed
# there. disable-libc-symbol-version is upstream's switch for static use.
build_fuse3() {
    echo "=== libfuse $FUSE_V ==="
    fetch "$FUSE_URL" "$FUSE_SHA" "$WORK/fuse-$FUSE_V.tar.gz"
    unpack "$WORK/fuse-$FUSE_V.tar.gz" "fuse-$FUSE_V"
    cd "$WORK/fuse-$FUSE_V"
    apply_patch . fuse-3.18.3-posix-memalign.patch
    meson setup _build --native-file "$WORK/native.ini" \
        --prefix="$SYSROOT" --libdir=lib --bindir=/usr/bin \
        -Dbuildtype=release \
        -Ddefault_library=static -Db_staticpic=true \
        -Dutils=false -Dexamples=false -Dtests=false \
        -Dinitscriptdir= -Denable-io-uring=false \
        -Ddisable-libc-symbol-version=true
    meson compile -C _build -j "$JOBS"
    meson install -C _build --no-rebuild
}

build_openssl() {
    echo "=== OpenSSL $OPENSSL_V ==="
    fetch "$OPENSSL_URL" "$OPENSSL_SHA" "$WORK/openssl-$OPENSSL_V.tar.gz"
    unpack "$WORK/openssl-$OPENSSL_V.tar.gz" "openssl-$OPENSSL_V"
    cd "$WORK/openssl-$OPENSSL_V"
    # OPENSSLDIR=/etc/ssl: the default CA lookup (cert.pem, certs/) matches
    # Debian-family hosts; elsewhere anyfs_tls_ca_init() sets SSL_CERT_FILE.
    perl ./Configure linux-x86_64 \
        --prefix="$SYSROOT" --libdir=lib --openssldir=/etc/ssl \
        no-shared no-module no-tests no-docs no-apps -fPIC
    make -j"$JOBS" build_libs
    make install_dev
}

# curl for QEMU's http(s) block driver. FTP stays: QEMU sets
# CURLOPT_PROTOCOLS_STR "HTTP,HTTPS,FTP,FTPS" and curl rejects the whole list
# if one protocol is compiled out. No CA bundle is compiled in (OpenSSL's
# defaults + SSL_CERT_FILE decide); the distro's openssl.cnf is not loaded,
# since it was written for a different OpenSSL build.
build_curl() {
    echo "=== curl $CURL_V ==="
    fetch "$CURL_URL" "$CURL_SHA" "$WORK/curl-$CURL_V.tar.xz"
    unpack "$WORK/curl-$CURL_V.tar.xz" "curl-$CURL_V"
    cd "$WORK/curl-$CURL_V"
    CFLAGS="-O2" ./configure \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --disable-shared --enable-static --with-pic \
        --disable-dependency-tracking \
        --with-openssl="$SYSROOT" --with-zlib \
        --without-ca-bundle --without-ca-path --with-ca-fallback --without-ca-embed \
        --disable-openssl-auto-load-config \
        --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
        --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
        --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
        --disable-manual --disable-docs \
        --without-libpsl --without-libidn2 --without-brotli --without-zstd \
        --without-nghttp2 --without-nghttp3 --without-ngtcp2 --without-quiche \
        --without-libssh --without-libssh2 --without-libgsasl --without-libuv \
        --without-zsh-functions-dir --without-fish-functions-dir
    make -j"$JOBS"
    make install
}

# ---------------------------------------------------------------------------
# Order: glib needs zlib + libffi; curl needs zlib + OpenSSL.
ALL_LIBS=(zlib bzip2 zstd libffi glib blkid libaio liburing fuse3 openssl curl)

run_one() {
    case "$1" in
        zlib|bzip2|zstd|libffi|glib|blkid|libaio|liburing|fuse3|openssl|curl)
            ( "build_$1" ) ;;
        *) echo "unknown --only target: $1 (one of: ${ALL_LIBS[*]})" >&2; exit 1 ;;
    esac
}

if [[ -n "$ONLY" ]]; then
    run_one "$ONLY"
else
    for lib in "${ALL_LIBS[@]}"; do run_one "$lib"; done
fi

list_libs() { find "$SYSROOT/lib" -maxdepth 1 -name '*.a' -printf '%f\n' | sort; }

echo
echo "=== manifest parity check ($SYSROOT) ==="
if diff <(grep -vE '^#|^$' "$SCRIPT_DIR/lib/linux_sysroot.manifest" | sort) <(list_libs); then
    echo "OK: sysroot lib set matches scripts/lib/linux_sysroot.manifest"
elif [[ -n "$ONLY" ]]; then
    echo "(partial build via --only=$ONLY — parity mismatch expected)"
else
    echo "FAIL: sysroot lib set differs from the manifest" >&2
    exit 1
fi
```

`chmod +x scripts/build_linux_sysroot.sh`. Each recipe runs in a subshell `( … )` so its
`cd` and variable changes don't leak into the next one.

- [ ] **Step 3: Write the manifest**

`scripts/lib/linux_sysroot.manifest`:

```
# scripts/lib/linux_sysroot.manifest — static libs scripts/build_linux_sysroot.sh
# installs under <linux_sysroot>/lib. The script's parity check diffs the
# built set against this list; CI's sysroot cache key includes this file.
libblkid.a
libbz2.a
libcrypto.a
libcurl.a
libffi.a
libfuse3.a
libgio-2.0.a
libgirepository-2.0.a
libglib-2.0.a
libgmodule-2.0.a
libgobject-2.0.a
libgthread-2.0.a
libpcre2-16.a
libpcre2-32.a
libpcre2-8.a
libpcre2-posix.a
libssl.a
libaio.a
liburing-ffi.a
liburing.a
libuuid.a
libz.a
libzstd.a
```

- [ ] **Step 4: Build it**

Run: `./scripts/build_linux_sysroot.sh --clean 2>&1 | tee ~/.cache/anyfs-linux-sysroot.log | tail -5`
Expected: `OK: sysroot lib set matches scripts/lib/linux_sysroot.manifest`. If the parity
diff shows extra/missing archives that are legitimate products (e.g. libaio installs
`libaio.a` only), correct the manifest, not the build.

- [ ] **Step 5: Prove the floor of every archive**

Link each archive whole into a throwaway shared object with `-z defs`, which fails on
any symbol the 2.11 stubs lack and proves every object is PIC:

```bash
S=$HOME/.cache/anyfs-linux-sysroot/x86_64-linux-gnu.2.11
for a in "$S"/lib/*.a; do
  scripts/lib/zig-cc -shared -o /dev/shm/whole.so -Wl,-z,defs \
    -Wl,--whole-archive "$a" -Wl,--no-whole-archive \
    $(PKG_CONFIG_LIBDIR=$S/lib/pkgconfig pkg-config --libs glib-2.0 gio-2.0 libcurl libzstd zlib libffi 2>/dev/null) \
    -L"$S/lib" -lbz2 -luuid -laio -lm \
    && scripts/check_linux_abi.sh 2.11 /dev/shm/whole.so >/dev/null \
    && echo "ok $a" || echo "FAIL $a"
done; rm -f /dev/shm/whole.so
```

Expected: `ok` for every archive except possibly `libblkid.a` + `libuuid.a` duplicates —
never whole-archive both at once (they embed identical helper objects by design); if
one of them fails only with "duplicate symbol", rerun it alone without `-luuid`.

- [ ] **Step 6: Lint and commit**

Add `scripts/build_linux_sysroot.sh` and `scripts/lib/sysroot_sources.sh` to the
shellcheck list, `scripts/build_linux_sysroot.sh` to `migrated`.

```bash
./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh
git add scripts/build_linux_sysroot.sh scripts/lib/linux_sysroot.manifest patches/sysroot \
        scripts/lint-shellcheck.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "build(linux): build a static dependency sysroot with zig at glibc 2.11"
```

---

## Task 6: LKL — kernel half on gcc, tools/lkl on zig

Spike facts (make 4.4.1 and a self-built make 4.3, full builds with a logging
dispatcher):
- The spec's "`-D__KERNEL__ -c` → gcc, everything else → zig" is wrong: Kconfig's
  `cc-version.sh`, `$(CC) --version`, `cc-option` probes, `-print-file-name` and the
  `vmlinux.lds` preprocessing also call `$(CC)`. Sent to zig, Kconfig would record clang.
- The kernel top Makefile exports `sub_make_done=1` before recursing into the `O=` dir;
  every process of the kernel build inherits it (recipes, sub-makes, parse-time
  `$(shell)`, make 4.3 and 4.4), and tools/lkl's own make never sets it. Measured: all
  2166 gcc-routed calls had it, none of the 42 zig calls did. The PATH entry
  `tools/lkl/bin` misses 208 kernel calls (the `.config` rule and the install sub-make);
  `KBUILD_CFLAGS` misses 37 under make 4.3; `srctree` is also exported by tools/lkl.
- Requires `O=`, which build_lkl.sh always sets (`OUTPUT`).
- Two gcc-isms zig rejects in tools/lkl: Makefile.autoconf's `find_include` runs
  `$(CC) -E -Wp,-v -xc /dev/null` (zig: unsupported preprocessor arg; VFIO_PCI and
  MACVTAP silently turn off), and Makefile.conf puts `-pie` on every link while the .so
  rules add `-shared` (gcc lets `-shared` win, zig errors). The wrapper handles both,
  since they are general gcc compatibility.
- gen_lkl_config.sh generates Makefile.conf by probing with the host gcc, which finds
  `/usr/include/fuse3` and enables lklfuse; probing through the dispatcher gives POSIX,
  VIRTIO_NET, VIRTIO_NET_FD, VFIO_PCI, VIRTIO_NET_MACVTAP (no FUSE: anyfs uses none of
  lklfuse/vfio/macvtap).
- Simulated Ubuntu-v3 gcc (`gcc -march=x86-64-v3`): without KCFLAGS, zstd fails with
  `immintrin.h: No such file or directory` exactly as in CI; with
  `KCFLAGS="-march=x86-64 -mtune=generic"`, none of `__AVX__ __AVX2__ __BMI__ __BMI2__
  __FMA__ __SSE4_2__` is defined and `lkl.o`'s section sizes equal the plain-gcc build.
- zig's lld links the gcc-built `ld -r` `lkl.o` into `liblkl.so` cleanly (91 dynamic
  exports, as before); max GLIBC_2.10; NEEDED libc, ld-linux, libpthread, librt. Host
  `ld -r`/`ar` handle the zig objects — don't pass LD/AR (they'd reach the kernel make).
- `tests/boot` 35/35 and `tests/disk -t ext4` 10/10 on the host and in squeeze.

**Files:**
- Modify: `scripts/lib/zig-cc.sh` (two gcc-compat rewrites)
- Create: `scripts/lib/lkl-linux-cc.sh`, `tests/test_lkl_linux_cc.sh`
- Modify: `scripts/build_lkl.sh`, `scripts/gen_lkl_config.sh`, `meson.build`, lint lists

- [ ] **Step 1: Teach the wrapper the two gcc-isms (test first)**

Append to `tests/test_zig_cc.sh`, before the final `echo OK`:

```bash
# gcc-isms: -Wp,-v (include-dir listing) and -pie together with -shared.
"$cc" -E -Wp,-v -xc /dev/null 2>&1 >/dev/null | grep -q '^ /' || fail "-Wp,-v lists no include dirs"
printf 'int lib(void){return 1;}\n' > "$tmp/lib.c"
"$cc" -fPIC -pie -shared "$tmp/lib.c" -o "$tmp/lib.so" || fail "-pie -shared rejected"
readelf -h "$tmp/lib.so" | grep -q 'DYN' || fail "-pie -shared did not produce a shared object"
```

Run `tests/test_zig_cc.sh`: expected FAIL at `-Wp,-v`.

In `scripts/lib/zig-cc.sh`, extend the header comment's list:

```sh
#   -Wp,-v              gcc's include-dir listing (LKL's Makefile.autoconf
#                       find_include); zig rejects it, clang's -v prints the
#                       same " <dir>" lines.
#   -pie with -shared   gcc lets -shared win (LKL's Makefile.conf puts -pie on
#                       every link); zig refuses the combination. Drop -pie.
```

and replace the `for a in "$@"; do … done` scan with:

```sh
shared=
for a in "$@"; do
    case $a in
    -dumpmachine)
        case $target in
        *-linux-gnu*) echo "${target%%-*}-unknown-linux-gnu" ;;
        *) echo "${target%%.*}" ;;
        esac
        exit 0
        ;;
    -print-search-dirs | --print-search-dirs)
        printf 'install: \nprograms: =\nlibraries: =\n'
        exit 0
        ;;
    -shared) shared=1 ;;
    esac
done
# Rewrite in place: each argument is shifted off the front and re-appended.
for a in "$@"; do
    shift
    case $a in
    -Wp,-v) a=-v ;;
    -pie) [ -n "$shared" ] && continue ;;
    esac
    set -- "$@" "$a"
done
```

Run `tests/test_zig_cc.sh`: expected `OK`.

- [ ] **Step 2: Write the failing dispatcher test `tests/test_lkl_linux_cc.sh`**

```bash
#!/usr/bin/env bash
# Gate for scripts/lib/lkl-linux-cc.sh: every call made inside the kernel
# build (sub_make_done=1, exported by the kernel's top Makefile) goes to gcc —
# compiles, probes, --version alike — and with --sccache only kernel compiles
# go through sccache. Everything else (tools/lkl's user-space half) goes to
# zig-cc. Uses stub compilers.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
wrapper="$root/scripts/lib/lkl-linux-cc.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

printf '#!/bin/sh\necho "sccache $*"\n' > "$tmp/sccache"
printf '#!/bin/sh\necho "gcc $*"\n' > "$tmp/gcc"
printf '#!/bin/sh\necho "zig $*"\n' > "$tmp/zig-cc"
chmod +x "$tmp/sccache" "$tmp/gcc" "$tmp/zig-cc"

check() {  # <want> <sub_make_done value or -> [--sccache] args...
    local want="$1" smd="$2" got
    shift 2
    if [[ "$smd" == - ]]; then
        got="$(env -u sub_make_done PATH="$tmp:$PATH" "$wrapper" "$@")"
    else
        got="$(sub_make_done="$smd" PATH="$tmp:$PATH" "$wrapper" "$@")"
    fi
    [[ "$got" == "$want" ]] || { echo "FAIL: smd=$smd $* -> '$got' (want '$want')"; exit 1; }
}
g="$tmp/gcc" z="$tmp/zig-cc"

# Inside the kernel build: gcc for everything.
check "gcc -D__KERNEL__ -c a.c -o a.o"  1 "$g" "$z" -D__KERNEL__ -c a.c -o a.o
check "gcc --version"                    1 "$g" "$z" --version
check "gcc -Werror -c -x c /dev/null -o t.o" 1 "$g" "$z" -Werror -c -x c /dev/null -o t.o
check "gcc -print-file-name=include"     1 "$g" "$z" -print-file-name=include
# --sccache: kernel compiles only; probes stay on plain gcc (sccache can't
# cache -o /dev/null).
check "sccache $g -D__KERNEL__ -c a.c -o a.o" 1 --sccache "$g" "$z" -D__KERNEL__ -c a.c -o a.o
check "gcc -Werror -c -x c /dev/null -o /dev/null" 1 --sccache "$g" "$z" -Werror -c -x c /dev/null -o /dev/null
# Outside it: zig.
check "zig -c lib/posix-host.c -o p.o"   - "$g" "$z" -c lib/posix-host.c -o p.o
check "zig -shared -o liblkl.so x.o"     - --sccache "$g" "$z" -shared -o liblkl.so x.o
check "zig -c a.c"                       0 "$g" "$z" -c a.c

echo "OK: lkl-linux-cc.sh routes the kernel build to gcc and tools/lkl to zig"
```

`chmod +x tests/test_lkl_linux_cc.sh`; run it: expected FAIL (script missing).

- [ ] **Step 3: Write `scripts/lib/lkl-linux-cc.sh`**

```sh
#!/bin/sh
# scripts/lib/lkl-linux-cc.sh — CC for the linux-amd64 LKL build.
#
# The kernel half (freestanding, -nostdinc) stays on the host gcc, ISA pinned
# by KCFLAGS in build_lkl.sh; tools/lkl's user-space half (posix-host.c, the
# liblkl.so link, tests) uses zig at the glibc floor. Every call made from
# inside the kernel build must reach gcc, not just compiles: Kconfig's
# cc-version.sh, `$(CC) --version` and cc-option probes would otherwise
# record zig's clang as the kernel compiler.
#
# The signal is sub_make_done=1: the kernel's top Makefile exports it before
# it recurses into the O= dir, so every process of the kernel build inherits
# it (recipes, sub-makes, parse-time $(shell), make 4.3 and 4.4), while
# tools/lkl's own make never sets it. build_lkl.sh always builds with O= and
# clears any stray value first.
#
# --sccache sends kernel compiles (-D__KERNEL__ … -c) through sccache, the
# bulk of the build; kernel probes stay on plain gcc (sccache mishandles
# -o /dev/null). zig-cc does its own sccache hand-off (ANYFS_ZIG_SCCACHE).
#
# Usage: CC="scripts/lib/lkl-linux-cc.sh [--sccache] <abs gcc> <abs zig-cc>"
sccache=
if [ "$1" = --sccache ]; then
    sccache=1
    shift
fi
gcc=$1
zigcc=$2
shift 2

if [ "${sub_make_done-}" != 1 ]; then
    exec "$zigcc" "$@"
fi
if [ -n "$sccache" ]; then
    kernel=
    compile=
    for a in "$@"; do
        case $a in
        -D__KERNEL__) kernel=1 ;;
        -c) compile=1 ;;
        esac
    done
    if [ -n "$kernel" ] && [ -n "$compile" ]; then
        exec sccache "$gcc" "$@"
    fi
fi
exec "$gcc" "$@"
```

`chmod +x scripts/lib/lkl-linux-cc.sh`. Run `tests/test_lkl_linux_cc.sh`: expected `OK`.

- [ ] **Step 4: Wire it into `scripts/build_lkl.sh`**

Replace `sccache_cc_for` with a `cc_for` that also covers the non-sccache linux-amd64
case:

```bash
# CC for target $1 (cross prefix $2). linux-amd64 always goes through
# lib/lkl-linux-cc.sh: the kernel half on the host gcc, tools/lkl on zig at
# the glibc floor. With --sccache the compilers go to sccache by absolute
# path, so sccache hashes and ships the real toolchain. mingw64 has two
# compilers (the tools/lkl/bin shim sends kernel code to cygwin-gcc), so it
# goes through lib/lkl-mingw-cc.sh; mingw32 has no shim (ILP32 long is
# pointer-sized) and uses its gcc directly. Prints nothing when the target
# keeps make's default CC.
cc_for() {
    local name="$1" cross="$2" cc scc=""
    if [[ "$name" == linux-amd64 ]]; then
        cc="$(command -v gcc)" || { echo "Error: gcc not on PATH" >&2; return 1; }
        [[ $USE_SCCACHE -eq 1 ]] && scc=" --sccache"
        echo "$SCRIPT_DIR/lib/lkl-linux-cc.sh$scc $cc $SCRIPT_DIR/lib/zig-cc"
        return
    fi
    [[ $USE_SCCACHE -eq 1 ]] || return 0
    if [[ "$name" == mingw64 ]]; then
        cc="$(command -v x86_64-pc-cygwin-gcc)" \
            || { echo "Error: x86_64-pc-cygwin-gcc not on PATH" >&2; return 1; }
        echo "$SCRIPT_DIR/lib/lkl-mingw-cc.sh $cc ${cross}gcc"
        return
    fi
    cc="$(command -v "${cross}gcc")" \
        || { echo "Error: ${cross}gcc not on PATH" >&2; return 1; }
    echo "sccache $cc"
}
```

In `build_one`, replace the `if [[ $USE_SCCACHE -eq 1 ]]; then … fi` block with:

```bash
    local scc
    scc="$(cc_for "$NAME" "$CROSS")" || return 1
    if [[ -n "$scc" ]]; then
        cc_arg=(CC="$scc")
        echo "  CC: $scc"
    fi
```

(`--cc` still overrides for other targets; for linux-amd64 an explicit `--cc` wins too,
so keep the `CC_OVERRIDE` assignment *after* this block:
`[[ -n "$CC_OVERRIDE" ]] && cc_arg=(CC="$CC_OVERRIDE")`.)

Extend the KCFLAGS block:

```bash
    # linux-amd64: pin the kernel half to the x86-64 baseline. GitHub's
    # ubuntu-26.04 images build gcc for amd64v3, which predefines __AVX2__;
    # LKL's zstd then includes <immintrin.h> under -nostdinc and fails, and
    # the rest would silently need AVX2. KCFLAGS comes last in kbuild, so it
    # wins over the compiler default. (The zig half pins its CPU by -target.)
    [[ "$NAME" == linux-amd64 ]] && kcflags_arg=(KCFLAGS="${KCFLAGS:+$KCFLAGS }-march=x86-64 -mtune=generic")
```

Before the `make` calls, for linux-amd64, clear a stray routing signal and point zig's
sccache hand-off at the same switch:

```bash
    # lkl-linux-cc.sh routes on sub_make_done (see there); a value inherited
    # from an outer kernel build would send tools/lkl to gcc.
    unset sub_make_done
    if [[ $USE_SCCACHE -eq 1 ]]; then export ANYFS_ZIG_SCCACHE=1; fi
```

Update the `--sccache` and `--cc` help lines: `--sccache` "Compile through sccache …;
for linux-amd64 both halves (kernel gcc via lkl-linux-cc.sh, zig via ANYFS_ZIG_SCCACHE)".

- [ ] **Step 5: Probe Makefile.conf through the dispatcher in `gen_lkl_config.sh`**

In the native POSIX branch, replace the Makefile.conf provocation with:

```bash
        # Probe with the compiler that will build tools/lkl. For linux-amd64
        # that's zig (via lib/lkl-linux-cc.sh): the host gcc would find e.g.
        # /usr/include/fuse3 and enable lklfuse, which zig then can't build.
        local probe_cc=()
        if [[ "$NAME" == linux-amd64 ]]; then
            probe_cc=(CC="$(dirname "$0")/lib/lkl-linux-cc.sh $(command -v gcc) $(cd "$(dirname "$0")" && pwd)/lib/zig-cc")
        fi
        rm -f "$CONF" "$LKL_OUT/include/lkl_autoconf.h"
        OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" ARCH=lkl "${probe_cc[@]}" \
             "$LKL_OUT/Makefile.conf" >/dev/null 2>&1 || true
```

Check the variable that holds the target name inside that function (`NAME`, `TARGET`
or `$1`) and the dispatcher path form (`gen_lkl_config.sh` may run with a relative
`$0`; absolutize with `$(cd "$(dirname "$0")" && pwd)` for both paths). Then verify:

```bash
./scripts/gen_lkl_config.sh --targets=linux-amd64
grep LKL_HOST_CONFIG lkl-linux-amd64/tools/lkl/Makefile.conf
```

Expected: POSIX, VIRTIO_NET, VIRTIO_NET_FD, VFIO_PCI, VIRTIO_NET_MACVTAP — and no FUSE.

- [ ] **Step 6: Build and verify**

```bash
./scripts/build_lkl.sh --targets=linux-amd64 --clean -j"$(nproc)"
scripts/check_linux_abi.sh 2.11 lkl-linux-amd64/tools/lkl/lib/liblkl.so
readelf -p .comment lkl-linux-amd64/tools/lkl/lib/liblkl.so | grep -E 'GCC|clang'
lkl-linux-amd64/tools/lkl/tests/boot
```

Expected: build OK; `ok … liblkl.so (GLIBC_2.10)`; both `GCC: … 14.x` (kernel) and
`clang version 21.1.0` (zig) appear in `.comment`; boot tests all pass.

- [ ] **Step 7: Register, lint, commit**

`meson.build`, next to `lkl_mingw_cc`:

```meson
    test('lkl_linux_cc', find_program('tests/test_lkl_linux_cc.sh'), suite: 'unit')
```

Add `scripts/lib/lkl-linux-cc.sh` to the shellcheck list.

```bash
./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh
git add scripts/lib/zig-cc.sh tests/test_zig_cc.sh scripts/lib/lkl-linux-cc.sh \
        tests/test_lkl_linux_cc.sh scripts/build_lkl.sh scripts/gen_lkl_config.sh \
        meson.build scripts/lint-shellcheck.sh
git commit -m "build(lkl): kernel half on gcc at the x86-64 baseline, tools/lkl on zig"
```

---

## Task 7: QEMU block layer on zig

Spike facts (QEMU 11.0.0 + the native patch series, out-of-tree build from a copy):
- **No QEMU source patches.** Configure flags only. All 199 glibc symbols referenced
  anywhere in the 7 archives resolve at 2.11; meson's link probes correctly turned off
  getrandom, gettid, memfd, getauxval, syncfs, copy_file_range, close_range,
  pthread_setname_np, …. The one compile-probe false positive (CONFIG_GETCPU) is only
  used by target/i386 code that isn't built.
- `libanyfs-qemublk.so` links with `-Wl,-z,defs` on the first try: max GLIBC_2.11,
  NEEDED libm/libc/ld-linux/libpthread/librt. A dlopen test registers 32 drivers on the
  host and in squeeze; qemu-img round-trips there.
- **Current build_qemu.sh leaks host libraries:** the post-configure
  `meson configure -Db_pie=false -Dwerror=false` makes the next `ninja` re-run meson in
  ninja's environment (no `PKG_CONFIG_LIBDIR`), which picked up host selinux, X11,
  xkbcommon and sysprof. Fix: export
  `PKG_CONFIG="env PKG_CONFIG_LIBDIR=<sysroot>/lib/pkgconfig pkg-config"` before
  configure — configure writes it into `config-meson.cross` (the native file it always
  passes to meson), so every regen stays on the sysroot — and use configure's own
  `--disable-pie --disable-werror` instead of the later `meson configure`.
- bzip2 has no `.pc`; without `--extra-cflags=-I<sysroot>/include
  --extra-ldflags=-L<sysroot>/lib` QEMU silently dropped the dmg-bz2 driver. The same
  `-I` is needed by the io_uring link probe (it passes no dependency). Use
  `--enable-bzip2 --enable-zstd --enable-linux-aio --enable-linux-io-uring` (curl is
  already `--enable-curl`) so a missing library fails configure instead.
- `--disable-vhost-user`: libvhost-user calls `memfd_create` (2.27) behind a
  `MFD_ALLOW_SEALING` guard that zig's kernel headers satisfy. It only breaks tool
  links (qemu-img), not the archives, but the flag keeps a full build clean.
- `--cxx` is needed (plugins make meson probe C++; nothing is compiled as C++).
  `--host-cc`/objcc are not.
- `pixman_*`: zero references in all 7 archives (zig and gcc builds alike).
- zig's lld rejects `--allow-multiple-definition`; keep `--whole-archive` on
  libblock.a only (whole-archiving all 7 gives real duplicates).
- `-dM -E` on the real qcow2.c line: none of AVX/SSE3/SSE4/BMI/FMA; QEMU's own
  `x86_version=1` adds `-mcx16 -msse2`. Post-v1 instructions only in CPUID-dispatched
  code (zstd `*_bmi2`, QEMU `buffer_zero_avx2`).
- Hazard noted for later code: `FD_SET` under `_FORTIFY_SOURCE` needs `__fdelt_chk`
  (2.15); zig's headers don't check the version. Nothing we build hits it.

**Files:**
- Modify: `scripts/build_qemu.sh`
- Modify: `meson.build` (pixman, already covered in Task 9 Step 3)

- [ ] **Step 1: linux-amd64 configure arguments**

In `configure_for()`, replace the `linux-amd64)` arm:

```bash
        linux-amd64)
            # zig at the glibc floor against the static sysroot. -I/-L: bzip2
            # has no .pc and QEMU's io_uring link probe passes no dependency,
            # so the sysroot must be on the default search path. --enable-*
            # makes a missing library fail configure instead of silently
            # dropping a driver. vhost-user calls memfd_create (glibc 2.27).
            # --disable-pie/--disable-werror replace a later `meson configure`,
            # which would make ninja re-run meson outside our environment.
            printf '%s\n' \
                "--cc=$SCRIPT_DIR/lib/zig-cc" \
                "--cxx=$SCRIPT_DIR/lib/zig-c++" \
                '--disable-pixman' '--disable-png' '--disable-vhost-user' \
                '--enable-bzip2' '--enable-zstd' \
                '--enable-linux-aio' '--enable-linux-io-uring' \
                '--disable-pie' '--disable-werror' \
                '--extra-cflags=-fPIC' \
                "--extra-cflags=-I$ANYFS_PATHS_LINUX_SYSROOT/include" \
                "--extra-ldflags=-L$ANYFS_PATHS_LINUX_SYSROOT/lib"
            ;;
```

The old comment about `-fPIC`/`b_pie` moves into this one; delete it.

- [ ] **Step 2: Pin pkg-config, recreate gcc-configured trees, skip `meson configure`**

In `build_one`, before the `if [[ ! -f "$builddir/build.ninja" ]]` block:

```bash
    # linux-amd64: pkg-config sees only the sysroot. configure writes
    # $PKG_CONFIG into config-meson.cross, the native file meson re-reads on
    # every regen, so a later ninja-triggered regen can't find host .pc files.
    local pkgcfg_env=()
    if [[ "$target" == linux-amd64 ]]; then
        pkgcfg_env=(PKG_CONFIG="env PKG_CONFIG_LIBDIR=$ANYFS_PATHS_LINUX_SYSROOT/lib/pkgconfig pkg-config")
        if [[ ! -f "$ANYFS_PATHS_LINUX_SYSROOT/lib/pkgconfig/glib-2.0.pc" ]]; then
            echo "ERROR: no linux sysroot at $ANYFS_PATHS_LINUX_SYSROOT — run scripts/build_linux_sysroot.sh" >&2
            return 1
        fi
        # A tree configured with another compiler can't switch in place.
        if [[ -f "$builddir/build.ninja" ]] && [[ -z "$CC_OVERRIDE" ]] \
           && ! grep -qF "$SCRIPT_DIR/lib/zig-cc" "$builddir/config-meson.cross" 2>/dev/null; then
            echo "  $builddir was not configured with zig-cc — recreating"
            rm -rf "$builddir"
        fi
    fi
```

Change the configure call to run under that environment:

```bash
        ( cd "$builddir" && env "${pkgcfg_env[@]}" "$QEMU_SRC/configure" \
              "${COMMON_CONFIGURE[@]}" "${target_cfg[@]}" "${cc_cfg[@]}" ) \
            || return 1
```

(`env` with an empty array runs the command unchanged.) Restrict the post-configure
`meson configure -Db_pie=false -Dwerror=false` to the mingw targets and say why:

```bash
        # mingw: set after configure (cross builds keep their own pkg-config
        # wrappers, so a regen can't leak host libraries there). linux-amd64
        # passes --disable-pie/--disable-werror to configure instead.
        if [[ "$target" != linux-amd64 ]]; then
            "$builddir/pyvenv/bin/meson" configure "$builddir" \
                -Db_pie=false -Dwerror=false || return 1
        fi
```

`--cc` (`CC_OVERRIDE`) still applies to linux-amd64; since `--cc=` appears after the
zig `--cc=` in the argument list, configure takes the override.

- [ ] **Step 3: Link libanyfs-qemublk.so with zig, statically, with `-z defs`**

`linker_cc_for`: `linux-amd64) echo "$SCRIPT_DIR/lib/zig-cc" ;;`.

Replace `pkgconfig_for` and the `pkg_libs` computation with a function that returns
the link flags:

```bash
# Link flags for the pkg-config modules of target $1. linux-amd64 resolves
# against the static sysroot (--static: Libs.private pulls OpenSSL into
# libcurl's line, etc.).
pkg_libs_for() {
    local target="$1"
    shift
    case "$target" in
        linux-amd64)
            PKG_CONFIG_LIBDIR="$ANYFS_PATHS_LINUX_SYSROOT/lib/pkgconfig" \
                pkg-config --static --libs "$@" ;;
        mingw32) i686-w64-mingw32-pkg-config --libs "$@" ;;
        mingw64) x86_64-w64-mingw32-pkg-config --libs "$@" ;;
    esac
}
```

and in `build_one`: `pkg_libs="$(pkg_libs_for "$target" "${pkg_mods[@]}")" || return 1`
(drop the `pkgcc` variable).

`pkg_modules_for` linux-amd64:
`printf '%s\n' "glib-2.0" "gthread-2.0" "zlib" "libzstd" "libcurl" "liburing"`
with the comment "pixman is disabled on every target; liburing is static in the sysroot
now, so the .so carries it instead of leaving it to consumers."

`extra_libs_for` linux-amd64:

```bash
        linux-amd64)
            # Static from the sysroot: libaio (no .pc) for native AIO, libbz2
            # (no .pc) for the dmg driver. -z defs: an unresolved symbol —
            # e.g. a glibc function above the floor — fails the link instead
            # of failing at load time.
            printf '%s\n' "-L$ANYFS_PATHS_LINUX_SYSROOT/lib" "-laio" "-lbz2" "-lm" "-Wl,-z,defs"
            ;;
```

Update the `pkg_modules_for` header comment: on linux the `.so` no longer relies on a
distro libcurl ("statically linked from the sysroot").

After the link, gate it:

```bash
    if [[ "$target" == linux-amd64 ]]; then
        "$SCRIPT_DIR/check_linux_abi.sh" 2.11 "$out" || return 1
    fi
```

Update the script header ("The host pkg-config (linux-amd64) … provide glib/zstd/zlib")
to: "linux-amd64 builds with scripts/lib/zig-cc against the static sysroot
(scripts/build_linux_sysroot.sh); mingw uses msys2-cross's per-target pkg-config."

- [ ] **Step 4: Build and verify**

```bash
./scripts/build_qemu.sh --targets=linux-amd64 --reconfigure -j"$(nproc)"
B=$HOME/qemu/build-anyfs-linux-amd64
grep -E '^(pkgconfig|c) =' "$B/config-meson.cross"
grep -E 'CONFIG_LINUX_AIO|CONFIG_LINUX_IO_URING|CONFIG_CURL|CONFIG_BZIP2|CONFIG_ZSTD|CONFIG_SELINUX|CONFIG_X11' "$B/config-host.h"
touch "$HOME/qemu/meson.build" && env -u PKG_CONFIG_LIBDIR ninja -C "$B" build.ninja \
  && ! grep -qE '/usr/(include|lib/x86_64-linux-gnu)' "$B/build.ninja" && echo regen-clean
```

Expected: `ok … libanyfs-qemublk.so (GLIBC_2.11)` from the gate; config-meson.cross
shows zig-cc and the `env PKG_CONFIG_LIBDIR=…` pkg-config; CONFIG_LINUX_AIO,
LINUX_IO_URING, CURL, BZIP2, ZSTD defined and SELINUX/X11 absent; `regen-clean`.
(`touch` on the shared ~/qemu tree only changes an mtime; it's fine.)

- [ ] **Step 5: Lint and commit**

```bash
./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh
git add scripts/build_qemu.sh
git commit -m "build(qemu): build the linux-amd64 block layer with zig against the sysroot"
```

---

## Task 8: anyfs core changes for the floor

**Files:**
- Create: `src/core/anyfs_tls.h`, `src/core/anyfs_tls.c`, `tests/unit/test_tls_ca.c`
- Modify: `src/core/qemu_thread.c`, `src/bench/shmem_relay_bench.c`, `meson.build`,
  `scripts/build_anyfs_wasm.sh`

- [ ] **Step 1: Write the failing unit test `tests/unit/test_tls_ca.c`**

```c
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the CA-bundle pick in src/core/anyfs_tls.c. */
#include "anyfs_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

int main(void)
{
	char present[] = "/tmp/anyfs-tls-ca-XXXXXX";
	int fd = mkstemp(present);
	CHECK(fd >= 0);
	close(fd);
	const char* const cands[] = {"/nonexistent/anyfs-ca.crt", present,
				     NULL};

	unsetenv("SSL_CERT_FILE");
	const char* got = anyfs_tls_ca_pick(cands);
	CHECK(got && strcmp(got, present) == 0);

	/* A user-supplied SSL_CERT_FILE always wins. */
	setenv("SSL_CERT_FILE", "/elsewhere.pem", 1);
	CHECK(anyfs_tls_ca_pick(cands) == NULL);
	unsetenv("SSL_CERT_FILE");

	const char* const none[] = {"/nonexistent/a", "/nonexistent/b", NULL};
	CHECK(anyfs_tls_ca_pick(none) == NULL);

	unlink(present);
	if (failures)
		return 1;
	printf("tls_ca: all checks passed\n");
	return 0;
}
```

Register in `meson.build` after `test('share_helpers', …)`:

```meson
test_tls_ca = executable('test_tls_ca',
    'tests/unit/test_tls_ca.c',
    'src/core/anyfs_tls.c',
    include_directories: [include_directories('src/core')],
    install: false,
)
test('tls_ca', test_tls_ca, suite: 'unit')
```

Run: `meson compile -C build-anyfs-linux-amd64 test_tls_ca`
Expected: FAIL (`anyfs_tls.h` not found).

- [ ] **Step 2: Write `src/core/anyfs_tls.h`**

```c
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * anyfs_tls.h — point the statically linked OpenSSL at the host's CA bundle.
 *
 * The linux-amd64 build links OpenSSL and curl statically with no CA bundle
 * compiled in. OpenSSL's defaults (OPENSSLDIR=/etc/ssl) cover Debian-family
 * hosts; RHEL/Fedora and SUSE keep the bundle elsewhere, and OpenSSL honours
 * SSL_CERT_FILE, so set that before the first TLS connection.
 */
#ifndef ANYFS_TLS_H
#define ANYFS_TLS_H

/* Returns the first readable path in the NULL-terminated `candidates`, or
 * NULL when SSL_CERT_FILE is already set or none exists. Exposed for tests. */
const char* anyfs_tls_ca_pick(const char* const* candidates);

/* Sets SSL_CERT_FILE to the host's bundle unless the user set it. Linux only;
 * a no-op elsewhere. Call once, before any thread may use TLS. */
void anyfs_tls_ca_init(void);

#endif
```

- [ ] **Step 3: Write `src/core/anyfs_tls.c`**

```c
// SPDX-License-Identifier: GPL-2.0-or-later
/* anyfs_tls.c — see anyfs_tls.h. */
#include "anyfs_tls.h"

#include <stdlib.h>
#include <unistd.h>

const char* anyfs_tls_ca_pick(const char* const* candidates)
{
	if (getenv("SSL_CERT_FILE"))
		return NULL;
	for (; *candidates; candidates++)
		if (access(*candidates, R_OK) == 0)
			return *candidates;
	return NULL;
}

void anyfs_tls_ca_init(void)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
	static const char* const bundles[] = {
		"/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Arch, Alpine */
		"/etc/pki/tls/certs/ca-bundle.crt",   /* RHEL, Fedora */
		"/etc/ssl/ca-bundle.pem",             /* SUSE */
		NULL,
	};
	const char* ca = anyfs_tls_ca_pick(bundles);
	if (ca)
		setenv("SSL_CERT_FILE", ca, 0);
#endif
}
```

- [ ] **Step 4: Call it before the QEMU thread starts**

QEMU's block/curl.c is the only TLS user, and it runs on the QEMU thread. In
`src/core/qemu_thread.c` add `#include "anyfs_tls.h"` after `#include "qemu_thread.h"`,
and in `qemu_thread_start()` inside `if (g_state == QT_IDLE) {`, before
`qemu_sem_init(&g_ready, 0);`:

```c
		/* setenv() races getenv() on other threads, so do it here,
		 * once, before the thread that will use TLS exists. */
		anyfs_tls_ca_init();
```

Add `'src/core/anyfs_tls.c',` to `anyfs_core_sources` in `meson.build` (after
`anyfs_share.c`) and `anyfs_tls.c` to `CORE_SOURCES` in `scripts/build_anyfs_wasm.sh`
(after `anyfs_share.c`).

- [ ] **Step 5: Replace `aligned_alloc` in the bench**

`src/bench/shmem_relay_bench.c` lines 252–255 (`aligned_alloc` is glibc 2.16):

```c
	/* posix_memalign, not aligned_alloc: the latter is glibc 2.16, above
	 * the linux-amd64 floor. */
	void* h2l_buf = NULL;
	void* l2h_buf = NULL;
	void* h2l_mem = NULL;
	void* l2h_mem = NULL;
	if (posix_memalign(&h2l_buf, 64, RING_CAP) ||
	    posix_memalign(&l2h_buf, 64, RING_CAP) ||
	    posix_memalign(&h2l_mem, 64, sizeof(struct ring_meta)) ||
	    posix_memalign(&l2h_mem, 64, sizeof(struct ring_meta))) {
		fprintf(stderr, "[relay] alloc failed\n");
		return -1.0;
	}
	struct ring_meta* h2l = h2l_mem;
	struct ring_meta* l2h = l2h_mem;
```

and delete the old `if (!h2l_buf || …)` block that followed.

- [ ] **Step 6: Run the unit suite (current gcc build dir is fine here)**

Run: `meson test -C build-anyfs-linux-amd64 tls_ca --print-errorlogs`
Expected: `tls_ca OK`.

- [ ] **Step 7: Commit**

```bash
git add src/core/anyfs_tls.c src/core/anyfs_tls.h tests/unit/test_tls_ca.c \
        src/core/qemu_thread.c src/bench/shmem_relay_bench.c meson.build \
        scripts/build_anyfs_wasm.sh
git commit -m "feat(core): point static OpenSSL at the host CA bundle"
```

---

## Task 9: anyfs on zig — `build_anyfs.sh` and `meson.build`

**Files:**
- Modify: `scripts/build_anyfs.sh`
- Modify: `meson.build`

- [ ] **Step 1: Generate the meson native file for linux-amd64**

In `scripts/build_anyfs.sh`, after `cross_file_for()`, add:

```bash
# linux-amd64 builds with zig against the static sysroot. Everything goes in a
# meson native file, not the environment: meson re-reads the file when ninja
# regenerates the build, while a PKG_CONFIG_LIBDIR from our environment would
# be gone and host .pc files would leak in.
native_file_for() {
    [[ "$1" == linux-amd64 ]] || return 0
    local sys="$ANYFS_PATHS_LINUX_SYSROOT" f="$SCRIPT_DIR/../.toolchain/meson-native-linux-amd64.ini"
    if [[ ! -f "$sys/lib/pkgconfig/glib-2.0.pc" ]]; then
        echo "ERROR: no linux sysroot at $sys — run scripts/build_linux_sysroot.sh" >&2
        return 1
    fi
    mkdir -p "$(dirname "$f")"
    cat > "$f" <<EOF
[binaries]
c = '$SCRIPT_DIR/lib/zig-cc'
cpp = '$SCRIPT_DIR/lib/zig-c++'
cmake = 'false'

[built-in options]
c_args = ['-I$sys/include']
c_link_args = ['-L$sys/lib']

[properties]
pkg_config_libdir = ['$sys/lib/pkgconfig']
EOF
    echo "$f"
}
```

In `build_one`, after `cross_file="$(cross_file_for "$target")"`:

```bash
    local native_file
    native_file="$(native_file_for "$target")" || return 1
```

Replace the `setup_extra` block with:

```bash
    local setup_extra=()
    [[ -n "$cross_file" ]] && setup_extra+=(--cross-file "$cross_file")
    [[ -n "$native_file" ]] && setup_extra+=(--native-file "$native_file")
    # A build dir configured with another compiler (the old host-gcc layout)
    # can't switch toolchains in place.
    if [[ -n "$native_file" && -f "$builddir/meson-private/cmd_line.txt" ]] \
       && ! grep -qF "$native_file" "$builddir/meson-private/cmd_line.txt"; then
        echo "  $builddir was configured without $native_file — recreating"
        rm -rf "$builddir"
    fi
```

- [ ] **Step 2: Use the sysroot's blkid and fuse3 on linux-amd64**

`blkid_root_for()`: return the sysroot for linux-amd64 (`<blkid/blkid.h>` resolves
against `$SYSROOT/include`, which blkid.pc's `-I…/include/blkid` does not provide):

```bash
        linux-amd64) echo "$ANYFS_PATHS_LINUX_SYSROOT" ;;
```

(replace the `linux-amd64|*) echo "" ;;` arm with that line plus `*) echo "" ;;`), and
update the comment above `blkid_root_for` and the `── core: hand-built libblkid` block's
comment to say linux-amd64 uses the sysroot's archive.

In the fuse block, test the sysroot instead of the host:

```bash
                if PKG_CONFIG_LIBDIR="$ANYFS_PATHS_LINUX_SYSROOT/lib/pkgconfig" \
                   pkg-config --exists fuse3 2>/dev/null; then
```

- [ ] **Step 3: Drop pixman from `meson.build`**

QEMU is configured `--disable-pixman` on linux-amd64 now (Task 7), and the tools-only
block layer never referenced it. In the `qemu_shared` linux branch delete
`pixman_dep = dependency('pixman-1', required: true)` and `pixman_dep,` from its
dependency list; in the static branch delete `pixman_dep = dependency('pixman-1', required: true)`
and `pixman_dep,`. Update the shared-branch comment
`# libanyfs-qemublk.so leaves libaio / liburing symbols undefined …` to:

```meson
            # libanyfs-qemublk.so carries libaio/liburing itself on the
            # zig build; listing them is harmless for older host-gcc trees.
```

- [ ] **Step 4: Build and run the unit suite**

```bash
./scripts/build_anyfs.sh --targets=linux-amd64 --components=core,server,fuse --reconfigure
meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs
scripts/check_linux_abi.sh 2.11 build-anyfs-linux-amd64/bin/
```

Expected: build succeeds; meson log shows `C compiler for the host machine: …/zig-cc (clang 21.1.0)`;
all unit tests OK (zig_cc / check_linux_abi run, not skipped); the ABI check prints
`ok` for anyfs-ksmbd, anyfs-nfsd, anyfs-lspart, anyfs-fuse with GLIBC ≤ 2.11.

- [ ] **Step 5: Run the Debian qcow2 smoke test**

Run: `./tests/smoke-debian-qcow2.sh --build-dir="$PWD/build-anyfs-linux-amd64" "$PWD/debian.qcow2"`
Expected: every `[PASS]`, no `[FAIL]` (the same result as the gcc build).

- [ ] **Step 6: Commit**

```bash
git add scripts/build_anyfs.sh meson.build
git commit -m "build(linux): build anyfs with zig against the static sysroot"
```

---

## Task 10: `package_linux.sh` bundles only our own libraries

**Files:**
- Modify: `scripts/package_linux.sh` (rewrite)

- [ ] **Step 1: Rewrite the script**

```bash
#!/bin/bash
# Package anyfs-reader for Linux amd64.
# Usage: ./scripts/package_linux.sh [builddir]   (default build-anyfs-linux-amd64)
#
# Everything is built with zig against the static sysroot (glibc 2.11 floor),
# so the tarball carries only our own shared libraries; check_linux_abi.sh
# gates every ELF before the tarball is written.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

BUILD_DIR="${1:-build-anyfs-linux-amd64}"
VERSION="${ANYFS_VERSION:-$(date +%Y%m%d)}"
PACKAGE_NAME="anyfs-reader-${VERSION}-linux-amd64"
OUT_DIR="${OUT_DIR:-/tmp}"
STAGING="$(mktemp -d)"
PKG="$STAGING/$PACKAGE_NAME"
trap 'rm -rf "$STAGING"' EXIT

LKL_SO="$SRC_DIR/lkl-linux-amd64/tools/lkl/lib/liblkl.so"
QEMU_SO="$ANYFS_PATHS_QEMU_SRC/build-anyfs-linux-amd64/libanyfs-qemublk.so"

echo "=== Packaging $PACKAGE_NAME ==="
[[ -d "$SRC_DIR/$BUILD_DIR" ]] || { echo "ERROR: $BUILD_DIR not found (run scripts/build_anyfs.sh)" >&2; exit 1; }
for f in "$LKL_SO" "$QEMU_SO"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (run build_lkl.sh / build_qemu.sh)" >&2; exit 1; }
done

meson install -C "$SRC_DIR/$BUILD_DIR" --destdir "$STAGING/install" >/dev/null
PREFIX="$(dirname "$(find "$STAGING/install" -name anyfs-ksmbd -type f -print -quit)")/.."

mkdir -p "$PKG/bin" "$PKG/lib"
for bin in anyfs-ksmbd anyfs-nfsd anyfs-lspart anyfs-fuse; do
    if [[ -f "$PREFIX/bin/$bin" ]]; then
        cp "$PREFIX/bin/$bin" "$PKG/bin/"
        echo "  bin/$bin"
    fi
done
cp -L "$LKL_SO" "$QEMU_SO" "$PKG/lib/"
echo "  lib/liblkl.so"
echo "  lib/libanyfs-qemublk.so"

# The libraries find each other next to themselves; give them plain sonames.
for so in "$PKG/lib/"*.so; do
    patchelf --set-rpath '$ORIGIN' "$so"
    patchelf --set-soname "$(basename "$so")" "$so"
done
for bin in "$PKG/bin/"*; do
    echo "  $(basename "$bin"): RUNPATH=$(readelf -d "$bin" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
done

echo "--- ABI gate (glibc 2.11, glibc-only NEEDED) ---"
"$SCRIPT_DIR/check_linux_abi.sh" 2.11 "$PKG"

tar czf "$OUT_DIR/$PACKAGE_NAME.tar.gz" -C "$STAGING" "$PACKAGE_NAME"
echo
echo "=== Package created: $OUT_DIR/$PACKAGE_NAME.tar.gz ==="
ls -lh "$OUT_DIR/$PACKAGE_NAME.tar.gz"
tar tzf "$OUT_DIR/$PACKAGE_NAME.tar.gz" | sort
```

The old script's `DT_NEEDED absolute path` rewrite is gone on purpose: the binaries link
LKL and QEMU statically, so no NEEDED entry carries a build path; the ABI gate would
reject one (it is not in the allowlist).

- [ ] **Step 2: Run it**

Run: `./scripts/package_linux.sh build-anyfs-linux-amd64`
Expected: four binaries + two libraries listed, `check_linux_abi: 6 ELF file(s) within GLIBC_2.11`,
the tarball path. `tar tzf` shows no `liburing`/`libaio`/other host library.

- [ ] **Step 3: Lint and commit**

Add `scripts/package_linux.sh` to the shellcheck list and `migrated`.

```bash
./scripts/lint-shellcheck.sh && ./scripts/lint-no-hardcoded-paths.sh
git add scripts/package_linux.sh scripts/lint-shellcheck.sh scripts/lint-no-hardcoded-paths.sh
git commit -m "build(linux): package only our own libraries, gated by check_linux_abi"
```

---

## Task 11: anyfs_native.node at glibc 2.25

**Files:**
- Create: `ts/packages/anyfs-native/exports.map`
- Modify: `ts/packages/anyfs-native/binding.gyp`
- Modify: `ts/packages/anyfs-native/scripts/build-linux-electron.sh`

- [ ] **Step 1: `exports.map`**

```
/* N-API entry points only. Wildcards: lld rejects a listed name the addon
 * doesn't define, and the version suffix differs across node-addon-api. */
{
  global:
    napi_register_module_v*;
    node_api_module_get_api_version_v*;
  local: *;
};
```

- [ ] **Step 2: `binding.gyp` linux block**

Add to `variables`:

```
    "linux_sysroot":  "<!(echo ${ANYFS_LINUX_SYSROOT:-${XDG_CACHE_HOME:-${HOME}/.cache}/anyfs-linux-sysroot/x86_64-linux-gnu.2.11})",
```

Remove `-DLKL_HOST_CONFIG_POSIX` from `cflags_c` (lkl_autoconf.h already defines it).
Replace the `OS=="linux"` condition body with:

```
        ["OS==\"linux\"", {
          # Built by scripts/build-linux-electron.sh with zig at glibc 2.25
          # (CC/CXX = scripts/lib/zig-c{c,++}, ANYFS_ZIG_TARGET). Every
          # dependency is a static archive from the linux sysroot, passed by
          # path so no host library can be picked up and QEMU's libcrypto.a
          # can't be confused with OpenSSL's.
          #
          # libanyfs_core.a was compiled with -DANYFS_HAS_QEMU, so it pulls in
          # the QEMU block layer. libblock.a needs --whole-archive: each
          # format driver registers itself from a constructor that nothing
          # references.
          "cflags":    ["-fvisibility=hidden"],
          "cflags_cc": ["-fvisibility-inlines-hidden"],
          "ldflags":   ["-Wl,--version-script=<(module_root_dir)/exports.map"],
          "libraries": [
            "-Wl,--start-group",
              "<(repo_root)/build-anyfs-linux-amd64/libanyfs_core.a",
              "<(repo_root)/lkl-linux-amd64/tools/lkl/liblkl.a",
              "-Wl,--whole-archive",
                "<(qemu_bld_linux)/libblock.a",
              "-Wl,--no-whole-archive",
              "<(qemu_bld_linux)/libio.a",
              "<(qemu_bld_linux)/libqom.a",
              "<(qemu_bld_linux)/libauthz.a",
              "<(qemu_bld_linux)/libcrypto.a",
              "<(qemu_bld_linux)/libevent-loop-base.a",
              "<(qemu_bld_linux)/libqemuutil.a",
              "<(linux_sysroot)/lib/libgio-2.0.a",
              "<(linux_sysroot)/lib/libgmodule-2.0.a",
              "<(linux_sysroot)/lib/libgobject-2.0.a",
              "<(linux_sysroot)/lib/libgthread-2.0.a",
              "<(linux_sysroot)/lib/libglib-2.0.a",
              "<(linux_sysroot)/lib/libpcre2-8.a",
              "<(linux_sysroot)/lib/libffi.a",
              "<(linux_sysroot)/lib/libblkid.a",
              "<(linux_sysroot)/lib/libcurl.a",
              "<(linux_sysroot)/lib/libssl.a",
              "<(linux_sysroot)/lib/libcrypto.a",
              "<(linux_sysroot)/lib/libzstd.a",
              "<(linux_sysroot)/lib/libbz2.a",
              "<(linux_sysroot)/lib/libz.a",
              "<(linux_sysroot)/lib/libaio.a",
              "<(linux_sysroot)/lib/liburing.a",
            "-Wl,--end-group",
            "-lpthread", "-lrt", "-ldl", "-lm"
          ]
        }]
```

Also default `qemu_bld_linux`, `qemu_src` and `linux_src` stay as they are (env
overridable).

- [ ] **Step 3: `build-linux-electron.sh` uses zig at 2.25 and gates the result**

Before the `npx node-gyp install` line:

```bash
# zig at glibc 2.25: code loaded into Electron never needs more than Electron
# itself (GLIBC_2.25), and zig links libc++ statically. LINK stays unset so
# node-gyp links with $(CXX) — zig c++ is what pulls libc++ in.
repo_root="$(cd ../../.. && pwd)"
export CC="$repo_root/scripts/lib/zig-cc" CXX="$repo_root/scripts/lib/zig-c++"
export ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.25
unset LINK
```

Replace `--arch=x64` line's command end with `--arch=x64 -j "$(nproc)"`, and after the
build:

```bash
"$repo_root/scripts/check_linux_abi.sh" --allow-undefined='^(napi_|node_api_)' \
    2.25 build/Release/anyfs_native.node
```

Update the header comment: replace the paragraph starting `So \`pnpm --filter
@anyfs/native build\` (host-targeted node-gyp rebuild) is sufficient` with:

```
# This script is the supported Linux build: it compiles with zig at glibc 2.25
# (see scripts/lib/zig-cc.sh) and gates the result with check_linux_abi.sh. A
# bare `node-gyp rebuild` would use the host gcc and link libstdc++ again.
```

- [ ] **Step 4: Build and load it in Electron**

```bash
cd ts/packages/anyfs-native && ./scripts/build-linux-electron.sh
E=../../examples/electron-demo/node_modules/electron/dist/electron
ELECTRON_RUN_AS_NODE=1 "$E" -e "const m=require('./build/Release/anyfs_native.node'); console.log(Object.keys(m).length > 0 ? 'loaded' : 'empty')"
readelf -d build/Release/anyfs_native.node | grep NEEDED
nm -D --defined-only build/Release/anyfs_native.node
```

Expected: `check_linux_abi: 1 ELF file(s) within GLIBC_2.25`; `loaded`; NEEDED has no
`libstdc++`/`libgcc_s`; exactly two defined dynamic symbols (`napi_register_module_v1`,
`node_api_module_get_api_version_v1`). Then run the spike's read-only Electron smoke:
`ELECTRON_RUN_AS_NODE=1 "$E" ~/.cache/anyfs-zig-spike/addon/real/smoke-ro.cjs "$PWD/build/Release/anyfs_native.node"`
— expected: partitions listed, ext2 mounted, `hello.txt` read, clean halt.

- [ ] **Step 5: Commit**

```bash
git add ts/packages/anyfs-native/exports.map ts/packages/anyfs-native/binding.gyp \
        ts/packages/anyfs-native/scripts/build-linux-electron.sh
git commit -m "build(native): build anyfs_native.node with zig at glibc 2.25"
```

---

## Task 12: drivelist.node at glibc 2.25 (outside this repo, left uncommitted)

**Files (in `~/drivelist-anyfs`):**
- Modify: `scripts/build-linux-electron.sh`
- Modify: `binding.gyp`
- Create: `exports.map` (same content as Task 11 Step 1)

- [ ] **Step 1: Build script**

In `scripts/build-linux-electron.sh`: default `ELECTRON_TARGET` from
`$ANYFS_READER/ts/examples/electron-demo/node_modules/electron/package.json` (fallback
`42.3.0`) instead of `33.4.11`, where `ANYFS_READER="${ANYFS_READER:-$(cd "$(dirname "$0")/../.." && pwd)/anyfs-reader}"`;
export `CC`/`CXX` = `$ANYFS_READER/scripts/lib/zig-c{c,++}`,
`ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.25`, `unset LINK`; finish with
`"$ANYFS_READER/scripts/check_linux_abi.sh" --allow-undefined='^(napi_|node_api_)' 2.25 build/Release/drivelist.node`.

- [ ] **Step 2: binding.gyp**

In the linux condition add `"cflags": ["-fvisibility=hidden"]`,
`"cflags_cc": ["-fvisibility-inlines-hidden"]`,
`"ldflags": ["-Wl,--version-script=<(module_root_dir)/exports.map"]`.

- [ ] **Step 3: Build and check**

```bash
cd ~/drivelist-anyfs && ./scripts/build-linux-electron.sh
E=~/anyfs-reader/ts/examples/electron-demo/node_modules/electron/dist/electron
ELECTRON_RUN_AS_NODE=1 "$E" -e "require('./build/Release/drivelist.node').list((e,d)=>console.log(e||d.length))"
```

Expected: the ABI gate passes at 2.25 with no libstdc++/libgcc_s; `list` prints a count
(0 is fine on Linux: enumeration is the JS lsblk path).

- [ ] **Step 4: Don't commit; record the diff for the report**

`git -C ~/drivelist-anyfs diff --stat` — list these files in the final summary.

---

## Task 13: CI — linux.yml

**Files:**
- Modify: `.github/workflows/linux.yml`

- [ ] **Step 1: Toolchain + sysroot steps, no link-only host packages**

Apt list: drop `libglib2.0-dev liburing-dev libaio-dev libzstd-dev libbz2-dev zlib1g-dev
libpixman-1-dev libfuse3-dev libnl-3-dev libnl-genl-3-dev libcurl4-openssl-dev`
(nothing links them now; leaving them out makes any host leak fail loudly). Keep
`build-essential` (kernel gcc), `libelf-dev libssl-dev` (kernel host tools),
`patchelf`, `perl` (OpenSSL Configure), test tools.

Delete the `Cache ksmbd-tools build` and `Build ksmbd-tools` steps: nothing consumes
`build-ksmbd-linux-amd64` (build_anyfs.sh compiles the ksmbd-tools sources itself), and
they were the only users of the libnl/glib -dev packages.

After `Install peru`, add:

```yaml
      # zig builds every linux-amd64 artifact (glibc floor + baseline ISA).
      # A stable install path keeps cached build trees valid across runs.
      - name: Cache zig
        id: cache-zig
        uses: actions/cache@v4
        with:
          path: ~/zig-0.16.0
          key: zig-0.16.0-x86_64-linux

      - name: Install zig
        run: ./scripts/fetch_zig.sh

      - name: Cache linux sysroot
        id: cache-sysroot
        uses: actions/cache@v4
        with:
          path: ~/.cache/anyfs-linux-sysroot
          key: linux-sysroot-${{ hashFiles('scripts/build_linux_sysroot.sh', 'scripts/lib/sysroot_sources.sh', 'scripts/lib/linux_sysroot.manifest', 'scripts/lib/zig-cc.sh', 'patches/sysroot/**', 'build.config.toml') }}

      - name: Build linux sysroot
        if: steps.cache-sysroot.outputs.cache-hit != 'true'
        run: ./scripts/build_linux_sysroot.sh
```

- [ ] **Step 2: sccache wiring**

After `Bring up sccache-dist farm`, add:

```yaml
      # Route zig compiles through sccache when the farm put it on PATH.
      - name: Enable sccache for zig
        run: |
          if command -v sccache >/dev/null 2>&1; then
            echo "ANYFS_ZIG_SCCACHE=1" >> "$GITHUB_ENV"
          fi
```

- [ ] **Step 3: Build steps**

LKL: replace the `CC_ARGS=(--cc="sccache gcc")` logic with `--sccache` when sccache is on
PATH (the dispatcher handles both halves), and drop the `liblkl.so` symlink lines into
`deps/linux/tools/lkl/lib` (package_linux.sh reads `lkl-linux-amd64` directly now).
QEMU: drop `CC_ARGS` entirely.

Cache keys — the toolchain changed, so old trees must not be restored:
- LKL: `key: lkl-linux-amd64-zig-${{ hashFiles('peru.yaml', 'scripts/gen_lkl_config.sh', 'scripts/build_lkl.sh', 'scripts/lib/lkl-linux-cc.sh', 'scripts/lib/zig-cc.sh', 'scripts/oot_fs.sh', '.github/workflows/linux.yml') }}`,
  `restore-keys: lkl-linux-amd64-zig-`.
- QEMU: `key: qemu-linux-amd64-zig-${{ hashFiles('peru.yaml', 'scripts/build_qemu.sh', 'scripts/lib/zig-cc.sh', 'patches/qemu/series.native', 'scripts/build_linux_sysroot.sh', 'scripts/lib/sysroot_sources.sh') }}`,
  `restore-keys: qemu-linux-amd64-zig-`.

- [ ] **Step 4: Packaging, ABI gate, old-userland smoke**

Replace the `Package distribution tarball` step body with
`./scripts/package_linux.sh build-anyfs-linux-amd64` (it runs the 2.11 ABI gate) and
`ls -la /tmp/anyfs-reader-*-linux-amd64.tar.gz`. Then add:

```yaml
      # glibc 2.11.3 userland: proves the floor at runtime, not only in the
      # symbol tables. Needs a host kernel that emulates vsyscall (glibc 2.11's
      # time() jumps into the vsyscall page); GitHub's Ubuntu kernels do.
      - name: Old-userland smoke (debian/eol:squeeze, glibc 2.11)
        run: |
          rm -rf /tmp/pkg && mkdir -p /tmp/pkg
          tar xzf /tmp/anyfs-reader-*-linux-amd64.tar.gz -C /tmp/pkg
          pkg="$(ls -d /tmp/pkg/anyfs-reader-*)"
          out="$(docker run --rm \
                   -v "$pkg:/anyfs:ro" \
                   -v /tmp/debian-13-genericcloud-amd64.qcow2:/img.qcow2:ro \
                   debian/eol:squeeze \
                   /anyfs/bin/anyfs-lspart /img.qcow2 2>&1)"
          echo "$out"
          echo "$out" | grep -qiE 'disk0/p1[[:space:]].*ext4'
          echo "$out" | grep -qiE 'disk0/p15[[:space:]].*(vfat|fat)'
```

Update the workflow's header comment (tarball contents, zig/sysroot).

- [ ] **Step 5: Validate the YAML and commit**

```bash
python3 -c 'import yaml,sys; yaml.safe_load(open(".github/workflows/linux.yml"))' && echo yaml-ok
git add .github/workflows/linux.yml
git commit -m "ci(linux): build with zig against the static sysroot, gate the ABI"
```

---

## Task 14: Docs, spec status, memory

**Files:**
- Modify: `docs/distribution.md`, the spec, auto-memory

- [ ] **Step 1: `docs/distribution.md`**

- Targets table, Linux row: `zig cc 0.16.0, target x86_64-linux-gnu.2.11 (glibc ≥ 2.11, x86-64 baseline); LKL kernel half on the host gcc`.
- Linux layout: `bin/` = anyfs-ksmbd, anyfs-nfsd, anyfs-lspart, anyfs-fuse; `lib/` =
  liblkl.so, libanyfs-qemublk.so only. Replace "System dependencies" with: "Only glibc
  ≥ 2.11 (libc, libm, libpthread, librt, libdl). Everything else is linked statically
  from the sysroot built by `scripts/build_linux_sysroot.sh`. anyfs-fuse uses the host's
  `/usr/bin/fusermount3` when not run as root."
- Runtime dependency matrix, Linux: drop the glib/liburing/libaio rows; add a note that
  TLS uses the host CA bundle (`anyfs_tls_ca_init`).
- Build-from-source section: `scripts/fetch_zig.sh` and `scripts/build_linux_sysroot.sh`
  before the LKL/QEMU/anyfs steps; remove the `$HOME/qemu/...so` and
  `-lglib-2.0 -lz -lzstd -luring -laio -lbz2` lines that describe the old host link.

- [ ] **Step 2: Spec status**

Set the spec's `**Status:**` to `implemented (2026-10-05)` and add a short
"Implementation notes" section listing the deviations: the LKL dispatcher routes the
whole kernel sub-make to gcc (not only `-D__KERNEL__ -c`); the wrapper's extra
`-UNDEBUG -fno-sanitize=undefined -g0` and `-print-search-dirs` handling; libresolv is
not needed; the squeeze smoke depends on vsyscall emulation.

- [ ] **Step 3: Memory**

Write `/home/kosaka/.claude/projects/-home-kosaka-anyfs-reader/memory/project_linux_zig_glibc_floor.md`
(type project) with the zig default-flag table, the sccache/`.toolchain/zig` design and
the per-library workarounds, and add it under "## Core / LKL (native)" in that
`MEMORY.md`. Add `feedback_zig_cc_defaults.md` (type feedback): zig cc injects NDEBUG/UBSan/
DWARF and leaks host lib dirs via -print-search-dirs — any zig cc wrapper must undo them.

- [ ] **Step 4: Commit**

```bash
git add docs/distribution.md docs/superpowers/specs/2026-10-05-linux-zig-glibc-floor-design.md
git commit -m "docs: linux-amd64 builds with zig at a glibc 2.11 floor"
```

---

## Landing

Local end-to-end run (Tasks 1–12) must pass before any push: sysroot, LKL, QEMU, anyfs,
unit suite, Debian qcow2 smoke, package + ABI gate, both addons in Electron.

Pushing: per `feedback_shared_tree_push_check.md`, inspect `git log origin/main..main`
for commits from other sessions and get the user's go-ahead before `git push`. Then watch
the linux workflow (`gh run watch`), including the squeeze smoke, which can only run on
the runner.
