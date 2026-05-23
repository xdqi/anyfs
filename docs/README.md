# anyfs-reader Documentation

## Index

| Document                                          | Description                                                                          |
| ------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [../README.md](../README.md)                      | Project overview, features, quick start, build instructions, benchmarks              |
| [ARCHITECTURE.md](ARCHITECTURE.md)                | Internal architecture, design decisions, I/O path, backend interface, QEMU details   |
| [lkl-servers.md](lkl-servers.md)                  | LKL file servers (`anyfs-ksmbd` + `anyfs-nfsd`): usage, kernel config, NFSv4, pynfs  |
| [anyfs-fuse.md](anyfs-fuse.md)                    | Native Linux LKL build and `anyfs-fuse` debugging notes                              |
| [anyfs-winfsp-plan.md](anyfs-winfsp-plan.md)      | WinFSP port: feasibility analysis and gap survey                                      |
| [winfsp-windows-fuse-plan.md](winfsp-windows-fuse-plan.md) | WinFSP port: step-by-step implementation plan                                |
| [win32-cross-compile.md](win32-cross-compile.md)  | Cross-compiling the whole stack for Windows i386 from Linux                          |
| [distribution.md](distribution.md)                | Build, packaging, and runtime-dependency matrix for Linux/Win32/Win64 releases       |
| [ts-packages.md](ts-packages.md)                  | TypeScript / browser packages (`@anyfs/core`, `react`, `trees`, `native`) and demos  |

## Language

All documentation is in English.

## Key Concepts

- **LKL** (Linux Kernel Library): runs actual Linux kernel filesystem code as a
  userspace library.
- **Backends**: pluggable block I/O layers (`raw`/`gio`/`qemu`) behind a single
  `anyfs_backend_ops` interface.
- **Synchronous I/O only**: every backend `request()` runs on the LKL virtio
  request path; async was evaluated and rejected
  (see [ARCHITECTURE.md §7](ARCHITECTURE.md)).
- **Thin C API**: `include/anyfs.h` exposes four functions (init/halt + disk
  add/remove); higher-level frontends use LKL syscalls directly.
- **`host_proxy` data path**: the ksmbd/nfsd servers do **not** route client
  traffic through libslirp; a host-side userspace TCP splice connects each
  accepted host connection to an LKL socket on `127.0.0.1`.
