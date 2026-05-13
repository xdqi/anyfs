# LKL Linux Native Build & FUSE Debugging Notes

## Overview

Building LKL natively for Linux (`liblkl.so` / `liblkl.a`) and linking anyfs-fuse against it.
The native build uses the LKL autoconf system (`Makefile.autoconf`) which detects the host
toolchain and generates `Makefile.conf`.

## Build System Architecture

LKL tools layer (`tools/lkl/`) has its own build system on top of the kernel Kbuild:

```
tools/lkl/Makefile.autoconf   →  detects host (POSIX/NT), generates Makefile.conf
tools/lkl/Makefile.conf       →  CROSS_COMPILE, CC, LD, AR, LKL_HOST_CONFIG_*, LDFLAGS
tools/lkl/include/lkl_autoconf.h  →  #define LKL_HOST_CONFIG_* from autoconf
tools/lkl/kernel.config       →  extra kernel CONFIG_* appended to .config
linux/.config                 →  full kernel Kconfig (ARCH=lkl)
```

Key outputs:

| File | Purpose |
|------|---------|
| `tools/lkl/lib/lkl.o` | Entire kernel as single relocatable object (~190MB) |
| `tools/lkl/lib/liblkl-in.o` | LKL host layer partial link (posix-host, virtio, net, etc.) |
| `tools/lkl/liblkl.a` | Static library: `liblkl-in.o` + `lkl.o` |
| `tools/lkl/lib/liblkl.so` | Shared library: `liblkl-in.o` + `lkl.o` linked with `-shared` |

## Native Build Procedure

### 1. Clean stale artifacts

If switching from a cross-compile (MinGW) back to native, remove everything:

```bash
cd ~/linux
rm -f .config .config.old vmlinux
rm -f arch/lkl/kernel/*.o
rm -f tools/lkl/Makefile.conf tools/lkl/kernel.config
rm -f tools/lkl/include/lkl_autoconf.h tools/lkl/tests/autoconf.sh
rm -f tools/lkl/lib/*.o tools/lkl/lib/*.d tools/lkl/lib/*.cmd
rm -f tools/lkl/liblkl.a tools/lkl/lib/liblkl.so tools/lkl/lib/liblkl.dll
```

### 2. Generate kernel config

```bash
cd ~/anyfs-reader
./scripts/gen_lkl_config.sh ~/linux
```

This script:
- Runs `make ARCH=lkl defconfig` for a fresh base
- Enables 35+ filesystems, networking, virtio, dm-crypt/LUKS, NFSDv4, KSMBD
- For native builds: removes stale `Makefile.conf` so LKL autoconf regenerates it
- For cross builds (`--cross=i686-w64-mingw32-`): writes `Makefile.conf` and `lkl_autoconf.h` manually

### 3. Build LKL

```bash
cd ~/linux
make -C tools/lkl -j$(nproc) ARCH=lkl
```

On first run, the autoconf system detects the native compiler and generates:
- `Makefile.conf` with `LKL_HOST_CONFIG_POSIX=y`, `-fPIC -pthread`, `SOSUF := .so`
- `include/lkl_autoconf.h` with `#define LKL_HOST_CONFIG_POSIX y`

The build may fail at tests (`disk-in.o`, `boot-in.o`) if stale `.d` dependency files
reference mingw paths. Core libraries (`lkl.o`, `liblkl-in.o`) are built before tests
and can be assembled manually:

```bash
cd tools/lkl
ar -rc liblkl.a lib/liblkl-in.o lib/lkl.o
gcc -shared -z noexecstack -fPIC -o lib/liblkl.so lib/liblkl-in.o lib/lkl.o -lrt -lpthread
```

### 4. Build anyfs-fuse

```bash
cd ~/anyfs-reader
meson setup build --reconfigure \
  -Dlkl_root=$HOME/linux/tools/lkl \
  -Dlkl_shared=true \
  -Denable_fuse=true
ninja -C build anyfs-fuse
```

## FUSE Segfault: MinGW Contamination

### Symptom

`anyfs-fuse` mounts successfully but segfaults on any FUSE callback that calls an LKL
syscall. Core dump shows thread 1 at address 0 (NULL function pointer from bad `longjmp`).

```
#0  0x0000000000000000 in ?? ()
#1  jmp_buf_longjmp () at jmp_buf.c
#2  __switch_to () at arch/lkl/kernel/threads.c
```

### Root cause

MinGW cross-compilation (`make -C tools/lkl CROSS_COMPILE=i686-w64-mingw32-`) at
2026-05-07 16:20 overwrote `liblkl.a`, `lkl.o`, and `liblkl-in.o` with COFF/PE32 objects.
The surviving `liblkl.so` was dynamically linked against these COFF objects → LKL's
`setjmp`/`longjmp` thread switching corrupted state on Linux.

`file` command confirmed:
```
lkl.o:        Intel i386 COFF    # WRONG — should be ELF 64-bit
liblkl-in.o:  Intel i386 COFF    # WRONG
posix-host.o: ELF 64-bit         # OK (survived from earlier Linux build)
```

The kernel `.config` was also contaminated:
```
CONFIG_CC_VERSION_TEXT="i686-w64-mingw32-gcc"
```

### Fix

Clean and rebuild with the native Linux toolchain (procedure above).

### Verification

The pre-built `${LINUX_SRC}/tools/lkl/lklfuse` (147MB, built 2026-05-07 15:52)
worked because it was statically linked against a Linux `liblkl.a` built before the
MinGW contamination. Our rebuilt `anyfs-fuse` matched this behavior after the rebuild.

```
$ anyfs-fuse disk_single.img /tmp/mnt -f -o backend=raw -o fstype=ext4
anyfs-fuse: disk_single.img mounted at /tmp/mnt (fstype: ext4, backend: raw)

$ ls -la /tmp/mnt
drwxr-xr-x  4 root root  1024 Apr 30 06:14 .
drwxr-xr-x  2 root root  1024 Apr 30 06:14 dir
-rw-r--r--  1 root root    11 Apr 30 06:14 hello.txt

$ cat /tmp/mnt/hello.txt
hello ext4

$ df -h /tmp/mnt
Filesystem  Size  Used Avail Use% Mounted on
anyfs-fuse   26M   49K   23M   1% /tmp/mnt
```

## Makefile.conf Reference

### Native Linux (POSIX)

```
export CROSS_COMPILE :=
export CC := gcc
export LD := ld
export AR := ar
export LKL_HOST_CONFIG_POSIX=y
export LKL_HOST_CONFIG_VIRTIO_NET=y
export LKL_HOST_CONFIG_VIRTIO_NET_FD=y
export LKL_HOST_CONFIG_VFIO_PCI=y
LDFLAGS += -pie -z noexecstack
CFLAGS += -fPIC -pthread
SOSUF := .so
LDLIBS += -lrt -lpthread
export LKL_HOST_CONFIG_FUSE=y
export LKL_HOST_CONFIG_VIRTIO_NET_MACVTAP=y
```

### MinGW Cross (NT32)

```
export CROSS_COMPILE := i686-w64-mingw32-
export CC := i686-w64-mingw32-gcc
export LD := i686-w64-mingw32-ld
export AR := i686-w64-mingw32-ar
export LKL_HOST_CONFIG_NT=y
KOPT = "KALLSYMS_EXTRA_PASS=1"
KOPT += "HOSTCFLAGS=-Wno-char-subscripts"
LDLIBS += -lws2_32
LDFLAGS += -Wl,--image-base,0x10000
EXESUF := .exe
SOSUF := .dll
CFLAGS += -Iinclude/mingw32
```

## gen_lkl_config.sh

Located at `scripts/gen_lkl_config.sh`. Usage:

```
./gen_lkl_config.sh [--cross=PREFIX] [--build] [LINUX_DIR]
```

For native builds, it removes stale `Makefile.conf` and `lkl_autoconf.h` so LKL's
autoconf regenerates them. For cross builds, it writes them manually because the
autoconf can't detect the target environment from the cross-compiler alone.
