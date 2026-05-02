# anyfs-reader

A library for reading filesystem images (ext4, btrfs, xfs, etc.) from userspace using [LKL](https://github.com/lkl/linux) (Linux Kernel Library). Supports raw disk images and QEMU-compatible formats (qcow2, vmdk, vdi) via linked QEMU block layer.

## Features

- Mount and read any Linux-supported filesystem without root privileges
- Multiple I/O backends:
  - **raw** — direct `pread()` on flat disk images
  - **gio** — GLib GIO `GInputStream` (cross-platform, synchronous)
  - **qemu** — QEMU block layer for qcow2/vmdk/vdi/vhd images
- Simple C API: open image → mount → read files/directories → unmount

## Building

### Prerequisites

- Linux (x86_64)
- Meson ≥ 0.60, Ninja
- Pre-built LKL (`liblkl.a`) from `linux/tools/lkl/`
- Optional: static GLib/GIO build for the GIO backend
- Optional: static QEMU block libs for QEMU backend

### Configure & Build

```bash
# Minimal (raw backend only)
meson setup builddir -Dlkl_root=$HOME/linux/tools/lkl

# With GIO backend
meson setup builddir -Dlkl_root=$HOME/linux/tools/lkl \
    -Dglib_root=$HOME/glib -Denable_gio=true

# With QEMU backend (qcow2/vmdk/vdi support)
meson setup builddir -Dlkl_root=$HOME/linux/tools/lkl \
    -Denable_qemu=true \
    -Dqemu_root=$HOME/qemu \
    -Dqemu_build=$HOME/qemu/build-anyfs2

ninja -C builddir
```

### Testing

```bash
# Create a test image
dd if=/dev/zero of=/tmp/test.img bs=1M count=32
mkfs.ext4 /tmp/test.img
mkdir -p /tmp/mnt && sudo mount /tmp/test.img /tmp/mnt
echo "Hello" | sudo tee /tmp/mnt/hello.txt
sudo umount /tmp/mnt

# Run test
./builddir/test_raw_mount /tmp/test.img ext4 0
```

## API Overview

```c
#include "anyfs_api.h"

AnyfsContext *ctx;
AnyfsMount *mnt;

anyfs_init(&ctx);
anyfs_open_image(ctx, "disk.img", ANYFS_OPEN_READONLY);
anyfs_mount(ctx, "ext4", 0, &mnt);

// Read files
anyfs_fd_t fd = anyfs_open(mnt, "/hello.txt", 0);
anyfs_read(mnt, fd, buf, sizeof(buf));
anyfs_close(mnt, fd);

// List directories
AnyfsDir *dir = anyfs_opendir(mnt, "/");
AnyfsEntry entry;
while (anyfs_readdir(dir, &entry) == ANYFS_OK) { ... }
anyfs_closedir(dir);

anyfs_umount(mnt);
anyfs_destroy(ctx);
```

## Project Structure

```
include/anyfs_api.h        — Public API header
src/core/anyfs_core.c      — Core logic (init, mount, VFS ops)
src/core/raw_blk_backend.c — pread-based block backend
src/core/gio_blk_backend.c — GIO synchronous backend
src/core/qemu_blk_backend.c — QEMU block backend
tests/                     — Test programs and benchmarks
```

## License

TBD
