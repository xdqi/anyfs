# Windows Cross-Compilation Notes (i686 / x86_64 MinGW)

## Overview

Cross-compile the whole stack (LKL kernel + QEMU block layer + anyfs-reader tools)
from a Linux host for Windows using the MinGW toolchains:

- Win32 (PE32 i386): `i686-w64-mingw32-*`
- Win64 (PE32+ amd64): `x86_64-w64-mingw32-*`

Both targets are tested under wine. End-to-end packaging is driven by
`scripts/build_lkl.sh` + `scripts/build_qemu.sh` + `scripts/build_anyfs.sh`. This
note focuses on the Windows-specific patches and gotchas; the user-facing build
flow lives in [distribution.md](distribution.md).

## Toolchain

- **Compiler**: `i686-w64-mingw32-gcc` / `x86_64-w64-mingw32-gcc` (GCC 14+,
  Debian package `gcc-mingw-w64`).
- **Patched binutils v2.46**: required for LKL's PE/COFF weak-symbol resolution.
  Standard mingw `ld` mishandles `.previous` directives and weak externals; the
  patched build lives under `$BINUTILS_DIR` (default
  `$HOME/binutils-gdb/build-combined/install/bin`). `scripts/gen_lkl_config.sh`
  wires absolute paths to `LD`/`AS`/`AR`/etc. into each target's
  `tools/lkl/Makefile.conf` so the LKL Kbuild uses the 2.46 binaries regardless
  of `$PATH` ordering. The 2.25.1 binaries shipped under
  `linux/tools/lkl/bin/` are below the 2.30 minimum kernel 6.13+ Kconfig
  requires and must not be used.
- **MSYS2 sysroot**: `/opt/msys2/mingw32/` and `/opt/msys2/mingw64/` provide
  the Windows builds of glib2, zstd, bzip2, zlib, intl, iconv, pcre2.

## 1. liblkl.dll (LKL kernel as a DLL)

### Locations after `scripts/build_lkl.sh`

- DLL: `lkl-<target>/tools/lkl/lib/liblkl.dll`
- Headers: `lkl-<target>/tools/lkl/include/`
- Win32 compat headers: `lkl-<target>/tools/lkl/include/mingw32/`
- Config: `lkl-<target>/.config` (i686/x86_64, 35 filesystems, ksmbd, nfsd v4,
  `DEBUG_INFO_NONE`).

### Key LKL patches applied for the Windows port

1. **`tools/lkl/lib/nt-host.c`: `page_alloc` / `page_free`**
   - Problem: kernel-image pages allocated by `malloc()` are non-executable and
     boot crashes the moment ksmbd's trampolines run.
   - Fix: `VirtualAlloc(PAGE_EXECUTE_READWRITE)` / `VirtualFree` replacements.

2. **`tools/lkl/lib/virtio_net_slirp.c`: Win32 compat layer**
   - POSIX → Win32 translations:
     - `socketpair()` via a loopback TCP connection
     - `WSAPoll()` for `poll()`
     - `ioctlsocket(FIONBIO)` for `fcntl(O_NONBLOCK)`
     - `QueryPerformanceCounter` for `clock_gettime`
     - `CreateThread` for `pthread_create`
     - `CRITICAL_SECTION` for `pthread_mutex`
   - Added `WSAStartup(MAKEWORD(2,2))` in `lkl_netdev_slirp_create()`.
   - The shipped ksmbd/nfsd binaries do **not** use slirp (host_proxy splice
     instead). The compat layer survives only because the rest of `virtio_net_*`
     pulls those declarations.

3. **`scripts/mod/Makefile`: `elfconfig.h` bypass**
   - Problem: `mk_elfconfig` can't parse PE/COFF objects.
   - Fix: comment out the `elfconfig.h` rule; pre-create the file with
     `ELFCLASS32` for Win32 or `ELFCLASS64` for Win64.

4. **`Makefile.autoconf`: NT host wiring**
   - Force-enable `VIRTIO_NET` for PE targets (otherwise the autoconf would
     skip it).

5. **`.config`: `DEBUG_INFO_NONE`**
   - `olddefconfig` re-enables `DEBUG_INFO` and the DLL balloons to 100 MB+.
     `gen_lkl_config.sh`'s overlay pins `DEBUG_INFO_NONE=y` after the
     `olddefconfig` pass.

### Build command (driven by helper scripts)

```bash
cd ~/anyfs-reader
./scripts/gen_lkl_config.sh \
    --linux=${LINUX_SRC} \
    --targets=mingw32,mingw64
./scripts/build_lkl.sh \
    --linux=${LINUX_SRC} \
    --targets=mingw32,mingw64 \
    -j$(nproc)
```

## 2. QEMU block backend DLL

- DLL: `~/qemu/build-anyfs-<target>/libanyfs-qemublk.dll` (~2 MiB stripped)
- Strategy: `libblock.a` as `--whole-archive`, the others in `--start-group`.
- Uses `--export-all-symbols` + `--enable-auto-import` so callers don't need
  `dllimport`.
- No pixman / liburing / libaio on Windows.

```bash
./scripts/build_qemu.sh \
    --targets=mingw32,mingw64 \
    --qemu-src=$HOME/qemu
```

Under the hood `build_qemu.sh` runs the QEMU configure (`--disable-system
--disable-user --enable-tools --disable-fuse --target-list=` and friends) and
then links the merged DLL with:

```bash
i686-w64-mingw32-gcc -shared -o libanyfs-qemublk.dll \
    -Wl,--export-all-symbols -Wl,--enable-auto-import \
    -Wl,--whole-archive libblock.a \
    -Wl,--no-whole-archive \
    -Wl,--start-group libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a \
                      libevent-loop-base.a -Wl,--end-group \
    -L/opt/msys2/mingw32/lib -lglib-2.0 -lintl -liconv -lzstd -lz -lbz2 \
    -lws2_32 -liphlpapi -lpathcch -lsynchronization -lwinmm -lpthread
i686-w64-mingw32-strip libanyfs-qemublk.dll
```

## 3. anyfs-reader (ksmbd + nfsd + lspart + winfsp)

Cross files: `scripts/cross-anyfs-mingw32.txt` and
`scripts/cross-anyfs-mingw64.txt`. They wire in the MSYS2 sysroot include/lib
paths plus the LKL Win32 compat headers.

```bash
./scripts/build_anyfs.sh \
    --targets=mingw32,mingw64 \
    --components=core,server,fuse \
    --qemu-root=$HOME/qemu \
    --ksmbd-root=$HOME/ksmbd-tools \
    --winfsp-root=$HOME/winfsp \
    -j$(nproc)
```

Outputs (per target):

- `build-anyfs-<target>/anyfs-ksmbd.exe` — SMB3 server (host_proxy data path)
- `build-anyfs-<target>/anyfs-nfsd.exe` — NFSv4 server (host_proxy data path)
- `build-anyfs-<target>/src/lspart/anyfs-lspart.exe` — partition lister
- `build-anyfs-<target>/anyfs-winfsp.exe` — WinFSP frontend (Win32 only)

`build-anyfs-<target>/bin/` is a symlink farm covering the full runtime closure
(LKL DLL, libanyfs-qemublk, MSYS2 DLLs, mingw runtime). `scripts/package_win32.sh`
and `scripts/package_mingw64.sh` dereference-copy from there.

## 4. Testing under wine

```bash
# liblkl boot tests (32 tests)
WINEPATH="$HOME/anyfs-reader/lkl-mingw64/tools/lkl/lib" wine tests/boot.exe

# host_proxy-backed ksmbd: serve disk.img, then connect from the host
WINEPATH="$HOME/anyfs-reader/build-anyfs-mingw64/bin" \
    wine build-anyfs-mingw64/anyfs-ksmbd.exe disk.img --share data=disk0/p1 &
smbclient //localhost/data -U guest%guest --port=4455 -c 'ls'
```

For why wine throughput on `anyfs-ksmbd.exe` is ~5–6× slower than native Linux
(it is, and that's wineserver IPC, not anyfs), use the `--busy-spin` flag — see
[lkl-servers.md](lkl-servers.md).

## 5. Known Issues

- `net-test.exe` fails to link (references `lkl_netdev_wintap_create` which is
  not built for 32-bit). It is a test binary; the DLL itself is fine.
- `lkl_pci lkl_pci: probe with driver lkl_pci failed` printed at startup is
  normal — wine has no PCI bus.
- Wine explorer error dialogs are cosmetic when launching `anyfs-winfsp.exe`
  without an actual WinFSP driver loaded.

## 6. Distribution layout

See [distribution.md §Layout](distribution.md). In short:

- Win32: `anyfs-reader-<ver>-win32.tar.gz`, flat layout with `.exe` + `.dll`
  side-by-side (no `bin/` / `lib/` split because Windows resolves `.dll` from
  the same directory).
- Win64: `anyfs-reader-<ver>-win64.tar.gz`, same flat layout.
