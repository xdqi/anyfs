# anyfs-reader Documentation

## Overview

| Document | Description |
|----------|-------------|
| [../README.md](../README.md) | Project overview, features, quick start, build instructions, benchmarks |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Internal architecture, design decisions, I/O path, backend interface, QEMU integration details |
| [lkl-servers.md](lkl-servers.md) | LKL file servers (ksmbd SMB3 + nfsd NFSv4): usage, kernel config, NFSv4 implementation, pynfs results |

## Language

- `README.md` (top-level) is in English.
- `docs/` files are primarily in Chinese (中文).

## Key Concepts

- **LKL** (Linux Kernel Library): Runs actual Linux kernel filesystem code as a userspace library.
- **Backends**: Pluggable block I/O layers (raw/gio/qemu) behind a single `anyfs_backend_ops` interface.
- **Synchronous I/O only**: All backends execute within the LKL virtio request path; async was evaluated and rejected (see ARCHITECTURE.md §7).
- **Thin API**: `anyfs.h` exposes only 4 functions (init/halt + disk add/remove); all file operations use LKL syscalls directly.
