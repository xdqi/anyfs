# Linux builds on zig cc with a pinned glibc floor and x86-64 baseline

**Date:** 2026-10-05
**Status:** approved direction (zig cc, two glibc floors, gcc for the kernel half, OpenSSL);
implementation in a separate session
**Scope:** linux-amd64 only — `scripts/build_lkl.sh`, `scripts/build_qemu.sh`, `scripts/build_anyfs.sh`,
`scripts/package_linux.sh`, new `scripts/lib/zig-cc.sh` / `scripts/build_linux_sysroot.sh` /
`scripts/check_linux_abi.sh`, `meson.build`, `ts/packages/anyfs-native` build, `.github/workflows/linux.yml`.
mingw64 (msys2-cross gcc, `-march=nocona`) and wasm (emcc) are unchanged.

## Problem

Every linux-amd64 artifact is built with whatever gcc the build host ships and links the host's
shared libraries. That ties the release to the build machine twice over:

1. **ISA.** GitHub's `ubuntu-26.04` runners are Ubuntu cloud images built for the `amd64v3`
   architecture variant, so the runner gcc targets x86-64-v3 by default and predefines `__AVX2__`.
   The first symptom was loud: LKL's zstd does `#if defined(__AVX2__)` → `#include <immintrin.h>`,
   which fails under the kernel's `-nostdinc` (LKL's `arch/lkl` has no `-mno-avx` like
   `arch/x86` does). The silent part is worse: QEMU, anyfs and the rest of LKL would compile
   cleanly into a tarball that only runs on AVX2 CPUs.
2. **glibc and shared libraries.** Measured on the current local build (Debian 13 host):

   | artifact | max `GLIBC_` | host `.so` it needs |
   |---|---|---|
   | `anyfs-ksmbd` | 2.38 | glib-2.0, blkid, bz2, zstd, aio, z, curl-gnutls, uring |
   | `libanyfs-qemublk.so` | 2.38 | glib-2.0, z, bz2, zstd |
   | `liblkl.so` | 2.34 | — |
   | `anyfs_native.node` | 2.38 | the above + libstdc++, libgcc_s |

   `package_linux.sh` copies only liburing and libaio into the tarball, so the rest must exist,
   at compatible versions, on the user's system. For comparison, Electron 42.3.0 itself (the
   `electron` binary plus every bundled `.so`) needs at most **GLIBC_2.25**.

## Goal

- Every linux-amd64 artifact targets the **x86-64 baseline** (v1) regardless of the build host.
- Two glibc floors, chosen by what loads the code:

  | what | zig target | why |
  |---|---|---|
  | code loaded into the Electron process: `anyfs_native.node`, the drivelist `.node` (drivelist-anyfs) | `x86_64-linux-gnu.2.25` | can never run where Electron itself can't; stock zig libc++ (no old-glibc patch) |
  | everything else: CLI binaries (ksmbd / nfsd / fuse / lspart), `liblkl.so`, `libanyfs-qemublk.so`, all static libraries | `x86_64-linux-gnu.2.11` | the same floor msys2-cross ships its Linux host tools at |

- The only shared libraries an artifact may need from the system are glibc's own
  (`libc.so.6`, `libm.so.6`, `libpthread.so.0`, `librt.so.1`, `libdl.so.2`,
  `ld-linux-x86-64.so.2`) plus the project's own bundled `liblkl.so` / `libanyfs-qemublk.so`.
- The build host's distro no longer affects the output, so the runners stay on `ubuntu-26.04`.

## Design

### 1. zig cc wrapper — `scripts/lib/zig-cc.sh`

- Modelled on msys2-cross `scripts/zig-common.sh`: thin `zig-cc` / `zig-c++` wrappers that run
  `zig cc|c++ -target "$ANYFS_ZIG_TARGET" "$@"` (default `x86_64-linux-gnu.2.11`), emulate
  `-dumpmachine` (zig rejects its own versioned triple; meson and autoconf call it), and strip
  nothing else.
- When sccache is on PATH and requested, the wrapper execs `sccache zig cc …`. The fork only
  recognises zig when the executable stem is literally `zig` and argv1 is `cc`/`c++`, so the
  wrapper must hand sccache `zig`, never itself. The target stays a visible argument, so the
  cache is partitioned by target for free.
- An explicit `-target` makes zig use the baseline CPU for that architecture (x86-64 v1). The
  wrapper test (below) asserts it rather than relying on it.
- zig version: pinned once in `build.config.toml`, same release msys2-cross's `prepare-zig.sh`
  defaults to (0.16.0). Stock zig is enough: the 2.11 side is C only, and msys2-cross's
  old-glibc libc++ patch is only needed for C++ below glibc 2.16. CI installs zig at a stable
  path (`$HOME/zig-<ver>`), not a per-run directory, so cached build trees stay valid.

### 2. Static dependency sysroot — `scripts/build_linux_sysroot.sh`

- Builds static, `-fPIC` archives with the 2.11 wrapper into
  `~/.cache/anyfs-linux-sysroot/<target>/` (not `/tmp`): zlib, bzip2, zstd, libffi, pcre2,
  glib (core + gobject + gio as QEMU and ksmbd-tools need), libblkid (from the util-linux peru
  dep), libaio, liburing, libfuse3, OpenSSL, libcurl.
- Pattern and versions follow `scripts/build_wasm_sysroot.sh` where the library is shared with
  wasm, so one version list covers both.
- Emits a `pkgconfig/` dir; every consumer builds with `PKG_CONFIG_LIBDIR` pointing only there,
  so a host `.pc` can never leak in.
- CI caches the sysroot keyed on the script and the version list.

**TLS / CA certificates.** curl links static OpenSSL, configured with no compiled-in bundle
(`--without-ca-bundle --without-ca-path --with-ca-fallback`, OpenSSL `OPENSSLDIR=/etc/ssl`).
OpenSSL's default lookup honours `SSL_CERT_FILE` / `SSL_CERT_DIR`, so a new
`anyfs_tls_ca_init()` in core, called once before the first curl use, sets `SSL_CERT_FILE` (if
unset) to the first existing well-known bundle: `/etc/ssl/certs/ca-certificates.crt`
(Debian/Ubuntu/Arch/Alpine), `/etc/pki/tls/certs/ca-bundle.crt` (RHEL/Fedora),
`/etc/ssl/ca-bundle.pem` (SUSE).

### 3. LKL — kernel half on gcc, host half on zig

- Kernel objects are freestanding (`-nostdinc`, no glibc), so they stay on the host gcc, the
  well-trodden kbuild path. The ISA is pinned with
  `KCFLAGS="-march=x86-64 -mtune=generic"` passed in `build_lkl.sh`'s environment. `KCFLAGS` is
  kbuild's documented variable for extra flags appended to every kernel compile.
- The tools/lkl user-space library (`posix-host.c` etc.) and the `liblkl.so` link move to zig
  2.11 through a dispatcher in the style of `scripts/lib/lkl-mingw-cc.sh`: kernel compiles
  (`-D__KERNEL__ … -c`) go to `sccache gcc`, everything else to `zig-cc`.

### 4. QEMU block layer — zig + sysroot

- `build_qemu.sh` linux-amd64: `--cc=zig-cc --cxx=zig-c++`, `PKG_CONFIG_LIBDIR` = sysroot.
  QEMU's own `x86_version=1` floor (`-mcx16 -msse2`) stays.
- Anything QEMU 11 hard-requires above glibc 2.11 becomes a small patch in
  `patches/qemu/series.native` (new patches only; shipped patches are never edited, see the
  retire mechanism in `scripts/lib/qemu_patches.sh`).

### 5. anyfs core and binaries

- `build_anyfs.sh` linux-amd64 sets `CC=zig-cc` and the sysroot `PKG_CONFIG_LIBDIR` for
  `meson setup`; `meson.build` drops the system-library assumptions (e.g. the
  `libaio.so.1t64` name) in favour of the sysroot's static deps.
- `package_linux.sh` stops copying host libraries (`BUNDLE_LIBS`); it bundles only
  `liblkl.so` and `libanyfs-qemublk.so`.

### 6. Electron-facing addons — zig at 2.25

- `anyfs_native.node`: node-gyp with `CC`/`CXX` set to the wrappers and
  `ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.25`. It links the same 2.11-built static archives
  (object files carry no symbol versions; the final link against the 2.25 stub picks them).
  zig links libc++ statically, so `libstdc++.so.6` / `libgcc_s.so.1` disappear from NEEDED.
- The drivelist `.node` gets the same treatment in drivelist-anyfs's build script.

### 7. Gates

- `tests/test_zig_cc.sh` (meson unit suite): the wrapper's `-dumpmachine` output; for both
  targets, `-dM -E` predefines none of `__AVX__`, `__AVX2__`, `__SSE4_2__`; a hello-world links
  and its max `GLIBC_` is ≤ the target.
- `scripts/check_linux_abi.sh <max-glibc> <files…>`: fails if any file's highest `GLIBC_`
  symbol version exceeds the floor, or if any `NEEDED` entry is outside the allowlist in Goal.
  linux.yml runs it on the packaged tarball (2.11) and wherever the addon is built (2.25).
- Old-userland smoke: linux.yml runs `anyfs-lspart` against the Debian qcow2 inside a
  `debian/eol:squeeze` container (glibc 2.11.3), which proves the floor at runtime and not
  only in symbol tables.
- These are build checks and stay in CI, consistent with "CI is build-only".

## Known risks

- **APIs newer than 2.11.** Functions above the floor fail at link time (zig's 2.11 stub libc
  omits them): e.g. `getrandom` (2.25), `memfd_create` (2.27), `pthread_setname_np` (2.12),
  `secure_getenv` / `getauxval` / `aligned_alloc` (2.16–2.17), `clock_gettime` living in librt
  before 2.17. glib, QEMU and util-linux probe most of these at configure time; anyfs's own
  calls get a raw-syscall fallback or a feature probe. The complete list only appears at the
  first full build.
- **Kernel features are a separate floor.** glibc 2.11-era systems run 2.6.32-era kernels.
  The libblkid spool's `O_TMPFILE` (kernel 3.11) gets a `mkstemp` + `unlink` fallback;
  io_uring stays optional at runtime. A minimum kernel version is not promised by this work.
- **Mixed toolchain link.** `liblkl.so` links a gcc-built relocatable `lkl.o` with zig's lld.
  Expected to work (it is a plain `ld -r` output), but it is the first thing to verify.
- **Static glib/gio.** gio has no module loading in a static build; QEMU's block layer and
  ksmbd-tools are expected not to need it.

## Implementation phases (separate session)

1. zig install + wrapper + sccache wiring + `test_zig_cc.sh`.
2. `build_linux_sysroot.sh` + CI cache.
3. LKL dispatcher + `KCFLAGS`.
4. QEMU on zig + sysroot (+ glibc-2.11 patches as needed).
5. anyfs meson on zig, `package_linux.sh`, `anyfs_tls_ca_init()`.
6. Addons at 2.25 (anyfs-native, drivelist-anyfs).
7. `check_linux_abi.sh` + squeeze smoke wired into linux.yml; docs and `doctor.sh` check for zig.

The linux job stays red on `ubuntu-26.04` (the zstd `immintrin.h` failure) until phases 3–5
land; from then on every phase must leave it green.

## Out of scope

- mingw64 and wasm builds.
- linux-arm64. It would follow the same scheme with `aarch64-linux-gnu.2.17` (the first glibc
  with aarch64) and is left to the arm64 work.
- A promised minimum kernel version; the runtime fallbacks above are best-effort.

## Acceptance criteria

1. On `ubuntu-26.04`, linux.yml builds, passes unit and smoke tests, and
   `check_linux_abi.sh 2.11` passes on every ELF in the tarball.
2. `anyfs_native.node` passes `check_linux_abi.sh 2.25` and loads in Electron 42.
3. `anyfs-lspart` lists the Debian qcow2's partitions inside `debian/eol:squeeze`.
4. `test_zig_cc.sh` shows no AVX/SSE4.2 predefines for either target.
5. The tarball contains no host-copied system libraries.
