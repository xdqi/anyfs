# anyfs-reader

Read any Linux-supported filesystem from userspace — no root, no FUSE, no kernel modules.

Uses [LKL](https://github.com/lkl/linux) (Linux Kernel Library) to run actual kernel filesystem code in a library. Supports raw disk images and QEMU-compatible formats (qcow2, vmdk, vdi) via linked QEMU block layer.

## Features

- Mount and read ext4, btrfs, xfs, f2fs, FAT, NTFS, etc. without root
- Multiple I/O backends:
  - **raw** — direct `pread()`, fastest (~1.3 GB/s)
  - **gio** — GLib GIO `GInputStream` (~1.1 GB/s)
  - **qemu** — QEMU block layer for qcow2/vmdk/vdi/vhd (~0.8 GB/s)
- Userspace SMB3 (`anyfs-ksmbd`) and NFSv4 (`anyfs-nfsd`) servers — a host-side TCP splice routes traffic into the in-LKL servers without libslirp on the data path
- FUSE frontend (`anyfs-fuse`) on Linux; WinFSP frontend (`anyfs-winfsp`) on Windows
- Minimal C API: 4 functions for kernel/disk management, then use LKL syscalls directly

## API

```c
#include "anyfs.h"
#include <lkl.h>
#include <lkl/linux/stat.h>

// 1. Start kernel
anyfs_kernel_init(NULL);  // or pass AnyfsKernelOpts

// 2. Add disk (auto-selects backend, or specify ANYFS_BACKEND_QEMU etc.)
int disk_id = anyfs_disk_add("disk.qcow2", ANYFS_DISK_READONLY);

// 3. Mount partition 1 — anyfs_mount picks an LKL mount point under /lklmnt/
AnyfsMount m;
anyfs_mount(disk_id, 1, "ext4", LKL_MS_RDONLY, NULL, &m);

// 4. Use LKL syscalls — full Linux VFS at your disposal
char path[256];
snprintf(path, sizeof path, "%s/etc/passwd", m.mount_point);
long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
lkl_sys_read(fd, buf, sizeof(buf));
lkl_sys_close(fd);

struct lkl_stat st;
lkl_sys_lstat(path, &st);

struct lkl_dir *dir = lkl_opendir(m.mount_point, &err);
// ...

// 5. Cleanup
anyfs_umount(m.mount_point);
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

anyfs-reader includes two userspace file servers that expose disk-image partitions over the network using LKL's in-tree ksmbd and nfsd subsystems. No root, no kernel modules; a host-side TCP splice (`src/host_proxy/`) routes client traffic into the in-LKL listener — libslirp is not on the data path.

| Server         | Protocol      | Host port (default) | Build option           |
| -------------- | ------------- | :-----------------: | ---------------------- |
| `anyfs-ksmbd`  | SMB3 (CIFS)   | 4455                | `-Denable_ksmbd=true`  |
| `anyfs-nfsd`   | NFSv4         | 20049               | (same flag)            |

```bash
# Build the servers
meson setup builddir-ksmbd -Dlkl_root=$HOME/linux/tools/lkl \
    -Denable_ksmbd=true -Dksmbd_tools_root=$HOME/ksmbd-tools
ninja -C builddir-ksmbd anyfs-ksmbd anyfs-nfsd

# Discover partition paths first
./builddir-ksmbd/src/lspart/anyfs-lspart disk.img

# SMB3 server
./builddir-ksmbd/anyfs-ksmbd disk.img --share data=disk0/p1
smbclient //localhost/data -U guest%guest --port=4455

# NFSv4 server
./builddir-ksmbd/anyfs-nfsd -w disk.img --share data=disk0/p1
mount -t nfs4 localhost:/data /mnt -o port=20049,vers=4
```

See [docs/lkl-servers.md](docs/lkl-servers.md) for kernel config, the `--share` DSL, NFSv4 implementation details, and pynfs results.

## Browser / Node packages

The `ts/` workspace ships the same anyfs stack as a wasm bundle plus thin JS
wrappers, so disk images can be inspected entirely client-side:

| Package          | Role                                                                          |
| ---------------- | ----------------------------------------------------------------------------- |
| `@anyfs/core`    | wasm kernel (LKL + QEMU block layer) + JS bindings; runs in a Web Worker      |
| `@anyfs/react`   | React 18/19 hooks (`useDir`, `useFile`) over `@anyfs/core`                    |
| `@anyfs/trees`   | `<AnyfsFileBrowser>` — Chonky-based file UI                                   |
| `@anyfs/native`  | Node N-API addon, links the native libs instead of wasm (Linux/macOS)         |

A live build of the browser demo (`ts/examples/vite-demo`) is hosted at
**<https://anyfs.kosaka.moe>** — drop a disk image onto the page or paste an
HTTP URL and the kernel boots in-browser. An Electron wrapper of the same demo
lives at `ts/examples/electron-demo`.

```bash
cd ts
pnpm install
pnpm -r --filter './packages/*' build
pnpm -F vite-demo dev    # http://localhost:5173
```

See [docs/ts-packages.md](docs/ts-packages.md) for the full layout, build
recipes, and the wasm-specific gotchas.

## Project Structure

```
include/anyfs.h              — Public API (4 functions + a few mount helpers)
include/anyfs_disk.h         — Multi-partition session layer
src/core/anyfs.c             — Kernel init + disk management
src/core/anyfs_disk.c        — Partition session + container recursion
src/core/raw_blk_backend.c   — pread-based block backend
src/core/gio_blk_backend.c   — GIO synchronous backend
src/core/qemu_blk_backend.c  — QEMU block backend bridge
src/host_proxy/              — Host-side TCP splice (used by ksmbd/nfsd)
src/ksmbd/lkl_ksmbd.c        — SMB3 server (LKL + ksmbd-tools)
src/nfsd/lkl_nfsd.c          — NFSv4 server (LKL nfsd + mini mountd)
src/fuse/anyfs_fuse.c        — FUSE3 frontend (Linux) and WinFSP frontend (Windows)
src/lspart/anyfs_lspart.c    — Partition lister
src/bench/                   — Benchmark / diagnostic utilities
tests/                       — Regression tests
docs/                        — Architecture, distribution, server docs
scripts/                     — gen_lkl_config / build_lkl / build_qemu / build_anyfs / package_*
```

## Documentation

See [docs/README.md](docs/README.md) for the full documentation index.

## License

GPL-2.0 — see [LICENSE](LICENSE).
