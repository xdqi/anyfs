# anyfs-reader

Read any Linux-supported filesystem from userspace — no root, no FUSE, no kernel modules.

Uses [LKL](https://github.com/lkl/linux) (Linux Kernel Library) to run actual kernel filesystem code in a library. Supports raw disk images and QEMU-compatible formats (qcow2, vmdk, vdi) via linked QEMU block layer.

## Features

- Mount and read ext4, btrfs, xfs, f2fs, FAT, NTFS, etc. without root
- Multiple I/O backends:
  - **raw** — direct `pread()`, fastest (~1.3 GB/s)
  - **gio** — GLib GIO `GInputStream` (~1.1 GB/s)
  - **qemu** — QEMU block layer for qcow2/vmdk/vdi/vhd (~0.8 GB/s)
- Interactive shell (`anyfs-shell`) with guestfish-like experience
- Minimal C API: 4 functions for kernel/disk management, then use LKL syscalls directly

## Quick Start

```bash
# Build (with QEMU backend)
meson setup builddir -Dlkl_root=$HOME/linux/tools/lkl \
    -Denable_qemu=true \
    -Dqemu_root=$HOME/qemu \
    -Dqemu_build=$HOME/qemu/build-anyfs2
ninja -C builddir

# Interactive shell
./builddir/anyfs-shell
><anyfs> open disk.qcow2
<disk.qcow2> mount ext4
<disk.qcow2:ext4 /> ls
<disk.qcow2:ext4 /> cat /etc/hostname
<disk.qcow2:ext4 /> df
<disk.qcow2:ext4 /> download /var/log/syslog ./syslog.txt

# Or one-liner
./builddir/anyfs-shell disk.img ext4
```

## Shell Commands

| Command | Description |
|---------|-------------|
| `open <image> [backend]` | Open disk image (backend: raw/gio/qemu, default: qemu) |
| `mount <fstype> [part]` | Mount filesystem |
| `umount` | Unmount |
| `ls [path]` | List directory |
| `ll [path]` | Long listing with file sizes |
| `cat <path>` | Print file contents |
| `head [-n N] <path>` | First N lines |
| `tail [-n N] <path>` | Last N lines |
| `stat <path>` | File info (type, size, inode, mode, uid/gid) |
| `hexdump <path> [off] [len]` | Hex dump |
| `find [path]` | Recursive file listing |
| `download <remote> <local>` | Copy file to host |
| `df` | Filesystem disk space usage |
| `cd <path>` | Change guest directory |
| `pwd` | Print guest working directory |
| `lcd <path>` | Change host directory |
| `!<cmd>` | Shell escape (run host command) |

Tab completion works for commands and guest filesystem paths.

## API

```c
#include "anyfs.h"
#include <lkl.h>
#include <lkl/linux/stat.h>

// 1. Start kernel
anyfs_kernel_init(NULL);  // or pass AnyfsKernelOpts

// 2. Add disk (auto-selects backend, or specify ANYFS_BACKEND_QEMU etc.)
int disk_id = anyfs_disk_add("disk.qcow2", ANYFS_DISK_READONLY);

// 3. Mount — use LKL directly
char mnt[32];
lkl_mount_dev(disk_id, 0, "ext4", LKL_MS_RDONLY, NULL, mnt, sizeof(mnt));

// 4. Use LKL syscalls — full Linux VFS at your disposal
long fd = lkl_sys_open("/mnt/.../etc/passwd", LKL_O_RDONLY, 0);
lkl_sys_read(fd, buf, sizeof(buf));
lkl_sys_close(fd);

struct lkl_stat st;
lkl_sys_lstat("/mnt/.../some/file", &st);

struct lkl_dir *dir = lkl_opendir("/mnt/...", &err);
// ...

// 5. Cleanup
lkl_umount_dev(disk_id, 0, 0, 1000);
anyfs_disk_remove(disk_id);
anyfs_kernel_halt();
```

## Building

### Prerequisites

- Linux (x86_64)
- Meson ≥ 0.60, Ninja
- Pre-built LKL (`liblkl.a`) from `linux/tools/lkl/`
- Optional: static GLib/GIO for GIO backend
- Optional: static QEMU block libs for QEMU backend

### Build Configurations

```bash
# Minimal (raw backend only)
meson setup builddir -Dlkl_root=$HOME/linux/tools/lkl

# Full (raw + GIO + QEMU)
meson setup builddir --default-library=static \
    -Dlkl_root=$HOME/linux/tools/lkl \
    -Dglib_root=$HOME/glib -Denable_gio=true \
    -Denable_qemu=true \
    -Dqemu_root=$HOME/qemu \
    -Dqemu_build=$HOME/qemu/build-anyfs2

ninja -C builddir
```

## Benchmarks

Reading ~150 MB of files from a 256 MB ext4 image:

| Backend | Raw image | qcow2 |
|---------|-----------|-------|
| raw (pread) | 1295 MB/s | — |
| gio | 1071 MB/s | — |
| qemu | 722 MB/s | 818 MB/s |

Run yourself: `./builddir/bench_backends <image> <fstype> [part]`

## File Servers

anyfs-reader includes two userspace file servers that expose a disk image over the network using LKL's built-in ksmbd and nfsd subsystems. Both use libslirp for userspace networking (no root required).

| Server | Protocol | Host Port | Build Option |
|--------|----------|-----------|--------------|
| `lkl_ksmbd` | SMB3 (CIFS) | 10445 | `-Denable_ksmbd=true` |
| `lkl_nfsd` | NFSv4 | 20049 | (same flag) |

```bash
# Build servers
meson setup builddir-ksmbd -Dlkl_root=$HOME/linux/tools/lkl -Denable_ksmbd=true
ninja -C builddir-ksmbd

# SMB3 server
./builddir-ksmbd/lkl_ksmbd -w disk.img
smbclient //localhost/share -p 10445 -N

# NFSv4 server
./builddir-ksmbd/lkl_nfsd -w disk.img
mount -t nfs4 localhost:/ /mnt -o port=20049,vers=4
```

See [docs/lkl-servers.md](docs/lkl-servers.md) for kernel config, NFSv4 implementation details, and pynfs test results.

## GUI File Manager

A GTK3-based graphical file browser (`anyfs-gui`) is available when GTK3 is installed. It provides a tree view of guest filesystem contents with read-only or read-write access.

```bash
# Requires gtk+-3.0
./builddir/anyfs-gui disk.img
./builddir/anyfs-gui -w disk.img   # read-write mode
```

## Project Structure

```
include/anyfs.h              — Public API (4 functions)
src/core/anyfs.c             — Kernel init + disk management
src/core/raw_blk_backend.c   — pread-based block backend
src/core/gio_blk_backend.c   — GIO synchronous backend
src/core/qemu_blk_backend.c  — QEMU block backend bridge
src/cli/shell.c              — Interactive shell (guestfish-like)
src/gui/anyfs_gui.c          — GTK3 GUI file manager
src/ksmbd/lkl_ksmbd.c       — SMB3 server (LKL + ksmbd-tools)
src/nfsd/lkl_nfsd.c         — NFSv4 server (LKL nfsd)
tests/                       — Tests and benchmarks
docs/                        — Architecture & design docs
```

## Documentation

See [docs/README.md](docs/README.md) for the full documentation index.

## License

TBD
