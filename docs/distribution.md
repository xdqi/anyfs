# Distribution

## Targets

| Platform | Arch          | Toolchain                                                              |
| -------- | ------------- | ---------------------------------------------------------------------- |
| Linux    | amd64         | `zig cc` 0.16.0, target `x86_64-linux-gnu.2.11` (glibc ≥ 2.11, x86-64 baseline); LKL kernel half on the host `gcc` |
| Windows  | i386 (Win32)  | `i686-w64-mingw32-` cross (MSYS2 headers/libs + binutils-2.46 patches) |
| Windows  | x86_64 (Win64)| `x86_64-w64-mingw32-` cross (MSYS2 headers/libs + binutils-2.46 patches)|

The mingw targets need the patched binutils-2.46 (LKL weak-symbol fixes) installed
into `$BINUTILS_DIR` (default `$HOME/binutils-gdb/build-combined/install/bin`). The
2.25.1 binutils shipped under `linux/tools/lkl/bin/` is below the 2.30 minimum kernel
6.13+ Kconfig requires, so `scripts/gen_lkl_config.sh` writes absolute `LD`/`AR`/etc.
paths into each per-target `Makefile.conf` to force the patched binutils.

linux-amd64 builds every artifact with `scripts/lib/zig-cc` (a wrapper around
`zig cc -target x86_64-linux-gnu.2.11` that undoes zig's non-gcc defaults; see the
header of `scripts/lib/zig-cc.sh`), so neither the build host's glibc nor its CPU
(GitHub's runners are x86-64-v3 images) reaches the output. The exception is the
LKL kernel: it is freestanding and stays on the host gcc with
`KCFLAGS=-march=x86-64`; `scripts/lib/lkl-linux-cc.sh` sends the whole kernel
sub-make to gcc and tools/lkl to zig. Third-party libraries come from a static
sysroot (`scripts/build_linux_sysroot.sh`), and `scripts/check_linux_abi.sh`
gates every shipped ELF. Code loaded into Electron (`anyfs_native.node`,
`drivelist.node`) targets `x86_64-linux-gnu.2.25`, Electron 42's own floor, with
libc++ linked statically.

## Binary naming

Every shipped executable uses the `anyfs-` prefix:

| Source target          | Distributed name      |
| ---------------------- | --------------------- |
| `anyfs-ksmbd`          | `anyfs-ksmbd[.exe]`   |
| `anyfs-nfsd`           | `anyfs-nfsd[.exe]`    |
| `anyfs-fuse`           | `anyfs-fuse` (Linux)  |
| `anyfs-winfsp`         | `anyfs-winfsp.exe`    |
| `anyfs-lspart`         | `anyfs-lspart[.exe]`  |

The retired binaries (`anyfs-shell`, `anyfs-gui`, the 7-Zip plugin) are not part of
any distribution.

## Layout

### Linux (`anyfs-reader-<version>-linux-amd64.tar.gz`)

```
anyfs-reader/
├── bin/
│   ├── anyfs-ksmbd        (RUNPATH = $ORIGIN/../lib)
│   ├── anyfs-nfsd
│   ├── anyfs-lspart
│   └── anyfs-fuse         (when built)
└── lib/
    ├── liblkl.so                  (LKL kernel + host library)
    └── libanyfs-qemublk.so        (QEMU block layer)
```

The binaries link LKL and the QEMU block layer statically; the two libraries are
shipped for embedders.

System dependencies: only glibc ≥ 2.11 (`libc`, `libm`, `libpthread`, `librt`,
`libdl`) on x86-64. glib, zlib, zstd, bzip2, libblkid, libaio, liburing, libfuse3,
OpenSSL and curl are linked statically from the sysroot. `anyfs-fuse` uses the
host's `/usr/bin/fusermount3` when not run as root. HTTPS images use the host's CA
bundle (OpenSSL's `/etc/ssl` defaults, or `SSL_CERT_FILE`, which anyfs points at
the RHEL/SUSE bundle when that is what exists).

### Windows (`anyfs-reader-<version>-win32.tar.gz` / `…-win64.tar.gz`)

```
anyfs-win{32,64}/
├── anyfs-ksmbd.exe
├── anyfs-nfsd.exe
├── anyfs-lspart.exe
├── anyfs-winfsp.exe       (Win32 only, requires WinFSP installed system-wide)
├── liblkl.dll
├── libanyfs-qemublk.dll
├── libglib-2.0-0.dll
├── libintl-8.dll
├── libiconv-2.dll
├── libpcre2-8-0.dll
├── libbz2-1.dll
├── libzstd.dll
├── zlib1.dll
├── libwinpthread-1.dll
├── libgcc_s_*.dll
└── libstdc++-6.dll
```

`libslirp-*.dll` is **not** shipped: `anyfs-ksmbd` and `anyfs-nfsd` use the
`host_proxy` TCP splice, and no other distributed binary imports slirp.

## Build Configuration

### Backend selection

Distribution builds enable **QEMU block backend only** (covers raw/qcow2/vmdk/vdi/vhd
etc.). The GIO and raw-only backends are not part of the dist set — they exist as
build options for development.

### LKL kernel config

`scripts/gen_lkl_config.sh` generates `lkl-<target>/.config` plus
`tools/lkl/Makefile.conf` and `tools/lkl/include/lkl_autoconf.h` for each requested
target. The overlay enables:

- ext2/ext3/ext4 (incl. journal), xfs, btrfs, vfat/exfat, ntfs3, f2fs, squashfs,
  iso9660, udf, hfsplus, minix, reiserfs, jfs, nilfs2, erofs (plus 15+ more — see
  the script).
- proc, sysfs, debugfs (LKL internals).
- nfsd v4 + ksmbd (server features).
- `CONFIG_DEBUG_INFO_NONE=y` to keep `liblkl.so` around 20 MiB.

**Do not edit `arch/lkl/configs/defconfig` or anything under `~/linux/` directly** —
all anyfs-specific overrides live in `gen_lkl_config.sh`'s `apply_common_config()`
overlay. Editing the kernel tree leaks state across targets.

### QEMU shared library

The shipped `libanyfs-qemublk.{so,dll}` bundles QEMU's `libblock.a`, `libqemuutil.a`,
`libio.a`, `libqom.a`, `libcrypto.a`, `libauthz.a`, and `libevent-loop-base.a` into a
single shared object.

Prereqs: QEMU is built with `-fPIC` + `b_pie=false` (linux-amd64: configure's
`--disable-pie`), and `util/fdmon-poll.c` is
patched to drop the `static` from its `static __thread` declarations. GCC always
uses local-exec TLS for `static __thread`, which produces `R_X86_64_TPOFF32`
relocations that cannot live in a shared library.

```bash
scripts/build_qemu.sh --targets=linux-amd64
# → <qemu_src>/build-anyfs-linux-amd64/libanyfs-qemublk.so
```

Internals of the merge (handled by `scripts/build_qemu.sh`; linux-amd64 links with
`scripts/lib/zig-cc` against the static sysroot, with `-z defs` so a symbol missing
at the glibc floor fails the link):

```bash
zig-cc -shared -o libanyfs-qemublk.so \
    -Wl,--whole-archive libblock.a \
    -Wl,--no-whole-archive \
    -Wl,--start-group libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a \
                      libevent-loop-base.a -Wl,--end-group \
    $(pkg-config --static --libs glib-2.0 gthread-2.0 zlib libzstd libcurl liburing) \
    -laio -lbz2 -lm -Wl,-z,defs
```

Use `--whole-archive` only on `libblock.a` — pulling `libqemuutil.a` whole forces
QMP command registration, which in turn drags in symbols only the full system
emulator provides. The remaining archives go in `--start-group` to resolve the
circular dependencies between them.

## Build Steps

### Phase 0 — linux-amd64 toolchain and sysroot

```bash
./scripts/fetch_zig.sh            # pinned zig → ~/zig-<version> (build.config.toml)
./scripts/build_linux_sysroot.sh  # static deps → ~/.cache/anyfs-linux-sysroot/…
```

### Phase 1 — LKL kernels (one tree per target)

```bash
cd ~/anyfs-reader
./scripts/gen_lkl_config.sh \
    --linux=${LINUX_SRC} \
    --targets=linux-amd64,mingw32,mingw64
./scripts/build_lkl.sh \
    --linux=${LINUX_SRC} \
    --targets=linux-amd64,mingw32,mingw64 \
    -j$(nproc)
```

Outputs: `lkl-<target>/tools/lkl/lib/liblkl.{a,so,dll}` for each requested target.

### Phase 2 — QEMU shared library

```bash
./scripts/build_qemu.sh \
    --targets=linux-amd64,mingw32,mingw64 \
    --qemu-src=$HOME/qemu
```

Produces `~/qemu/build-anyfs-<target>/libanyfs-qemublk.{so,dll}`.

### Phase 3 — anyfs-reader binaries

```bash
./scripts/build_anyfs.sh \
    --targets=linux-amd64,mingw32,mingw64 \
    --components=core,server,fuse \
    --qemu-root=$HOME/qemu \
    --ksmbd-root=$HOME/ksmbd-tools \
    --winfsp-root=$HOME/winfsp \
    -j$(nproc)
```

Per target this drives:

- `meson setup build-anyfs-<target>` with the right `lkl_dist`, `enable_*`, and
  cross-file flags.
- `meson compile -C build-anyfs-<target>`.

Missing optional inputs (no ksmbd-tools, no WinFSP, …) skip the affected
components with a warning rather than failing the whole build.

### Phase 4 — Package

```bash
# Linux (runs check_linux_abi.sh 2.11 on every ELF before writing the tarball)
./scripts/package_linux.sh build-anyfs-linux-amd64
# /tmp/anyfs-reader-<YYYYMMDD>-linux-amd64.tar.gz

# Win32
./scripts/package_win32.sh 0.1.0
# /tmp/anyfs-reader-0.1.0-win32.tar.gz

# Win64
./scripts/package_mingw64.sh 0.1.0
# /tmp/anyfs-reader-0.1.0-win64.tar.gz
```

Each script dereference-copies the binaries, strips them, sets `RUNPATH=$ORIGIN`
on Linux, and tars the output.

## Runtime Dependency Matrix

### Linux

| Library                 | Source       | Notes                                |
| ----------------------- | ------------ | ------------------------------------ |
| `liblkl.so`             | self-built   | LKL kernel + nfsd + ksmbd            |
| `libanyfs-qemublk.so`   | self-built   | QEMU block layer                     |
| glibc ≥ 2.11            | system       | the only system dependency           |

### Windows

| DLL                     | Source              |
| ----------------------- | ------------------- |
| `liblkl.dll`            | self-built (mingw)  |
| `libanyfs-qemublk.dll`  | self-built (mingw)  |
| `libglib-2.0-0.dll`     | MSYS2 cross         |
| `libintl-8.dll`         | MSYS2 cross         |
| `libiconv-2.dll`        | MSYS2 cross         |
| `libpcre2-8-0.dll`      | MSYS2 cross         |
| `libbz2-1.dll`          | MSYS2 cross         |
| `libzstd.dll`           | MSYS2 cross         |
| `zlib1.dll`             | MSYS2 cross         |
| `libwinpthread-1.dll`   | mingw-w64 runtime   |
| `libgcc_s_*.dll`        | mingw-w64 runtime   |
| `libstdc++-6.dll`       | mingw-w64 runtime   |

WinFSP must be installed system-wide (`winfsp.msi` from <https://winfsp.dev>) before
running `anyfs-winfsp.exe`.
