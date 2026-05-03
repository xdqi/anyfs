# Win32 (i686-w64-mingw32) Cross-Compilation Notes

## Overview

Cross-compile the entire anyfs-reader stack (LKL kernel + libslirp + anyfs-reader tools) for
Windows 32-bit (PE32 i386) from a Linux host. Tested under Wine.

## Toolchain

- **Compiler**: `i686-w64-mingw32-gcc` (GCC 14, Debian package `gcc-mingw-w64-i686`)
- **Patched Binutils**: v2.25.1 with LKL-specific patches for PE/COFF weak symbol resolution
  - Source: `~/binutils-gdb` (built at `~/mingw-patched/bin/`)
  - Deployed to: `~/linux/tools/lkl/bin/` (ld, objcopy)
  - Required because standard mingw `ld` can't handle `.previous` directives and weak symbols in LKL
- **MSYS2 libraries**: `/opt/msys2/mingw32/` (glib2, zstd, bzip2, zlib, readline, intl, iconv, pcre2)

## 1. liblkl.dll (LKL Kernel as DLL)

### Location
- DLL: `~/linux/tools/lkl/lib/liblkl.dll` (16MB, stripped)
- Headers: `~/linux/tools/lkl/include/`
- Win32 compat headers: `~/linux/tools/lkl/include/mingw32/` (empty sys/socket.h stub)
- Config: `~/linux/.config` (i686, 35 filesystems, ksmbd, nfsd v4, DEBUG_INFO_NONE)

### Key Patches Applied to LKL

1. **nt-host.c: page_alloc/page_free**
   - Problem: `malloc()` memory is not executable, kernel crashes on boot
   - Fix: Added `VirtualAlloc(PAGE_EXECUTE_READWRITE)` / `VirtualFree` implementations

2. **virtio_net_slirp.c: Win32 compat layer**
   - Ported POSIX pipe/poll/fcntl/clock_gettime to Win32:
     - `socketpair()` via loopback TCP connection
     - `WSAPoll()` instead of `poll()`
     - `ioctlsocket(FIONBIO)` instead of `fcntl(O_NONBLOCK)`
     - `QueryPerformanceCounter` instead of `clock_gettime`
     - `CreateThread` instead of `pthread_create`
     - `CRITICAL_SECTION` instead of `pthread_mutex`
   - Added `WSAStartup(MAKEWORD(2,2))` in `lkl_netdev_slirp_create()`

3. **scripts/mod/Makefile: elfconfig.h bypass**
   - Problem: `mk_elfconfig` can't parse PE/COFF objects
   - Fix: Commented out the elfconfig.h rule, pre-create with ELFCLASS32

4. **Makefile.autoconf: Enable VIRTIO_NET + VIRTIO_NET_SLIRP for pe-i386**
   - Modified `nt_host` section to include slirp CFLAGS/LDLIBS for 32-bit NT

5. **.config adjustments**
   - Run `scripts/config -d DEBUG_INFO -e DEBUG_INFO_NONE` before build
   - `olddefconfig` re-enables DEBUG_INFO otherwise → bloats DLL to 100MB+

### Build Command
```bash
cd ~/linux/tools/lkl
make mrproper
cp ~/anyfs-reader/configs/lkl_defconfig .config
# Remove CONFIG_64BIT line (auto-detect from compiler)
sed -i '/CONFIG_64BIT/d' .config
make olddefconfig CROSS_COMPILE=i686-w64-mingw32- ARCH=lkl
scripts/config -d DEBUG_INFO -e DEBUG_INFO_NONE
make -j$(nproc) CROSS_COMPILE=i686-w64-mingw32- ARCH=lkl
```

### Dependencies (DLLs needed at runtime)
- `libslirp-0.dll` (from libslirp cross-build)
- `libglib-2.0-0.dll` (MSYS2, needed by libslirp)
- `libintl-8.dll` (MSYS2, needed by glib)
- `libiconv-2.dll` (MSYS2, needed by intl)
- `libpcre2-8-0.dll` (MSYS2, needed by glib)
- `libgcc_s_dw2-1.dll` (mingw runtime)
- `libwinpthread-1.dll` (mingw runtime)

## 2. libslirp-0.dll (User-mode Networking)

### Location
- DLL: `~/libslirp/build-mingw32/libslirp-0.dll` (952KB)
- Import lib: `~/libslirp/build-mingw32/libslirp.dll.a`
- Pkg-config: `~/libslirp/build-mingw32/slirp.pc`
- Headers: `~/libslirp/build-mingw32/include/slirp/`

### Build Command
```bash
cd ~/libslirp
meson setup build-mingw32 --cross-file <cross-file> -Ddefault_library=shared
ninja -C build-mingw32
```

## 3. anyfs-reader (Shell + ksmbd + nfsd)

### Cross-file
- `~/anyfs-reader/cross-win32.txt`
- Includes paths to MSYS2 headers/libs, libslirp build dir, LKL mingw32 compat headers

### Meson Options
```bash
meson setup builddir-win32 --cross-file cross-win32.txt \
  -Dlkl_root=${LINUX_SRC}/tools/lkl \
  -Dlkl_shared=true \
  -Denable_qemu=true \
  -Denable_gio=false \
  -Denable_ksmbd=true \
  -Dksmbd_tools_root=${KSMBD_TOOLS_SRC}
```

### Executables
- `anyfs-shell.exe` — Interactive filesystem shell (readline)
- `anyfs-ksmbd.exe` — SMB3 file server (slirp networking, no root)
- `anyfs-nfsd.exe` — NFSv4 file server (slirp networking, no root)
- No `anyfs-gui.exe` (GTK3 is Linux-only)

### QEMU Block Backend
- DLL: `~/qemu/build-win32/libanyfs-qemublk.dll` (2.0MB stripped)
- Strategy: `libblock.a` as `--whole-archive`, others in `--start-group`
- Uses `--export-all-symbols` + `--enable-auto-import` (no dllimport needed)
- No pixman/liburing/libaio on Windows
- Configure command:
```bash
cd ~/qemu && mkdir build-win32 && cd build-win32
PKG_CONFIG_LIBDIR=$HOME/qemu/build-win32-pkgconfig \
../configure --cross-prefix=i686-w64-mingw32- --cpu=i386 \
    --disable-system --disable-user --enable-tools \
    --disable-guest-agent --disable-docs --disable-gtk --disable-sdl \
    --disable-opengl --disable-vnc --disable-spice --disable-gnutls \
    --disable-blkio --disable-numa --disable-cap-ng --disable-seccomp \
    --disable-libssh --disable-curl --disable-rbd --disable-glusterfs \
    --disable-vde --disable-nettle --disable-gcrypt --disable-smartcard \
    --disable-usb-redir --disable-libudev --disable-fuse \
    --disable-libiscsi --disable-libnfs --disable-pixman --disable-png \
    --target-list= \
    --extra-cflags="-I/opt/msys2/mingw32/include" \
    --extra-ldflags="-L/opt/msys2/mingw32/lib"
ninja libblock.a libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a

# Link into DLL
i686-w64-mingw32-gcc -shared -o libanyfs-qemublk.dll \
    -Wl,--export-all-symbols -Wl,--enable-auto-import \
    -Wl,--whole-archive libblock.a \
    -Wl,--no-whole-archive \
    -Wl,--start-group libqemuutil.a libio.a libqom.a libcrypto.a libauthz.a libevent-loop-base.a -Wl,--end-group \
    -L/opt/msys2/mingw32/lib -lglib-2.0 -lintl -liconv -lzstd -lz -lbz2 \
    -lws2_32 -liphlpapi -lpathcch -lsynchronization -lwinmm -lpthread
i686-w64-mingw32-strip libanyfs-qemublk.dll
```

## 4. Testing Under Wine

```bash
# All 32 boot tests pass:
WINEPATH="$HOME/linux/tools/lkl/lib-win32" wine tests/boot.exe

# Echo server with slirp networking:
WINEPATH="$HOME/linux/tools/lkl/lib-win32" wine lkl_echo_slirp.exe
echo "hello" | nc localhost 12345  # ← works!
```

## 5. Known Issues

- `net-test.exe` fails to link (references `lkl_netdev_wintap_create` which isn't built for 32-bit)
  - This is just a test binary, not affecting liblkl.dll functionality
- Wine explorer errors are cosmetic (no GUI needed)
- `lkl_pci lkl_pci: probe with driver lkl_pci failed` — normal (no PCI bus in Wine)

## 6. File Layout for Distribution

```
anyfs-win32/
├── bin/
│   ├── anyfs-shell.exe
│   ├── anyfs-ksmbd.exe
│   └── anyfs-nfsd.exe
├── lib/
│   ├── liblkl.dll
│   ├── libslirp-0.dll
│   ├── libglib-2.0-0.dll
│   ├── libintl-8.dll
│   ├── libiconv-2.dll
│   ├── libpcre2-8-0.dll
│   ├── libgcc_s_dw2-1.dll
│   └── libwinpthread-1.dll
└── etc/
    └── ksmbd.conf.example
```
