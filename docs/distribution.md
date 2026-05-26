# Distribution

## Targets

| Platform | Arch          | Toolchain                                                              |
| -------- | ------------- | ---------------------------------------------------------------------- |
| Linux    | amd64         | Native `gcc`                                                            |
| Windows  | i386 (Win32)  | `i686-w64-mingw32-` cross (MSYS2 headers/libs + binutils-2.46 patches) |
| Windows  | x86_64 (Win64)| `x86_64-w64-mingw32-` cross (MSYS2 headers/libs + binutils-2.46 patches)|

The mingw targets need the patched binutils-2.46 (LKL weak-symbol fixes) installed
into `$BINUTILS_DIR` (default `$HOME/binutils-gdb/build-combined/install/bin`). The
2.25.1 binutils shipped under `linux/tools/lkl/bin/` is below the 2.30 minimum kernel
6.13+ Kconfig requires, so `scripts/gen_lkl_config.sh` writes absolute `LD`/`AR`/etc.
paths into each per-target `Makefile.conf` to force the patched binutils.

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
│   └── anyfs-fuse         (when built)
└── lib/
    ├── liblkl.so                  (~20 MiB, 35 filesystems + nfsd + ksmbd)
    ├── libanyfs-qemublk.so        (~10 MiB, QEMU block layer)
    ├── liburing.so.2
    └── libaio.so.1t64
```

System dependencies (not bundled — assumed present on target):

- `libglib-2.0`, `libz`, `libzstd`, `libbz2`, `libm`, `libc`.

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

Prereqs: QEMU is built with `-fPIC` + `b_pie=false`, and `util/fdmon-poll.c` is
patched to drop the `static` from its `static __thread` declarations. GCC always
uses local-exec TLS for `static __thread`, which produces `R_X86_64_TPOFF32`
relocations that cannot live in a shared library.

```bash
# 1) Build QEMU (PIC)
scripts/build_qemu.sh \
    --targets=linux-amd64 \
    --qemu-src=$HOME/qemu \
    --out-prefix=build-anyfs

# 2) The script produces:
#    $HOME/qemu/build-anyfs-linux-amd64/libanyfs-qemublk.so
```

Internals of the merge (handled by `scripts/build_qemu.sh`):

```bash
gcc -shared -o libanyfs-qemublk.so \
    -Wl,--whole-archive libblock.a \
    -Wl,--no-whole-archive \
    -Wl,--start-group libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a \
                      libevent-loop-base.a -Wl,--end-group \
    -lglib-2.0 -lz -lzstd -luring -laio -lbz2
```

Use `--whole-archive` only on `libblock.a` — pulling `libqemuutil.a` whole forces
QMP command registration, which in turn drags in symbols only the full system
emulator provides. The remaining archives go in `--start-group` to resolve the
circular dependencies between them.

## Build Steps

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
# Linux
./scripts/package_linux.sh build-anyfs-linux-amd64
# /tmp/anyfs-reader-<YYYYMMDD>-linux-amd64-pkg/  →  *.tar.gz

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
| `libglib-2.0.so`        | system pkg   | GLib (used by QEMU block)            |
| `liburing.so.2`         | system pkg   | QEMU io_uring backend                |
| `libaio.so.1t64`        | system pkg   | QEMU linux-aio backend               |

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
