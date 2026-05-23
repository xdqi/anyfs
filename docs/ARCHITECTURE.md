# anyfs-reader Architecture

## 1. Design Decisions

| Decision         | Choice                                | Rationale                                                                   |
| ---------------- | ------------------------------------- | --------------------------------------------------------------------------- |
| API style        | Thin shim + direct LKL syscalls       | The old per-call wrapper was a meaningless 1:1 passthrough                  |
| Host operations  | Stock posix-host (no GLib substitute) | Keeps the LKL host layer untouched                                          |
| QEMU integration | Static-link `libblock.a` directly     | `blk_pread()` is synchronous; zero LKL modifications                        |
| I/O model        | **Synchronous only**                  | LKL's async story is too fragile (see §7)                                   |
| MMU              | `CONFIG_MMU=n`                        | Filesystems do not need an MMU                                              |
| LKL patches      | **None on the data path**             | Pure synchronous I/O works as shipped; server tweaks live in §3 of [lkl-servers.md](lkl-servers.md) |

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│ Applications                                            │
│   anyfs-ksmbd / anyfs-nfsd / anyfs-fuse /               │
│   anyfs-winfsp / anyfs-lspart / bench_backends / ...    │
│   call LKL syscalls directly:                           │
│     lkl_sys_open, lkl_sys_read, lkl_sys_lstat,          │
│     lkl_opendir, lkl_sys_statfs, ...                    │
├─────────────────────────────────────────────────────────┤
│ include/anyfs.h           — 4 fn: init/halt + add/remove│
│ include/anyfs_disk.h      — multi-partition session API │
├─────────────────────────────────────────────────────────┤
│ libanyfs_core.a                                         │
│   src/core/anyfs.c             kernel lifecycle + disks │
│   src/core/anyfs_disk.c        partition session layer  │
│   src/core/raw_blk_backend.c   pread() backend (.img)   │
│   src/core/gio_blk_backend.c   GIO synchronous backend  │
│   src/core/qemu_blk_backend.c  QEMU blk_pread (qcow2…)  │
│   src/core/kindprobe.c         partition kind probe     │
│   src/core/path_dsl.c          partition path parser    │
├─────────────────────────────────────────────────────────┤
│ liblkl.{a,so,dll} (LKL 6.18, CONFIG_MMU=n)              │
├─────────────────────────────────────────────────────────┤
│ libblock.a + libqemuutil.a (QEMU block layer, optional) │
└─────────────────────────────────────────────────────────┘
```

### API Design Philosophy

The earlier API wrapped `anyfs_open/read/close/opendir/readdir/closedir` etc. — over a
dozen functions that were 1:1 forwards to `lkl_sys_*` with no added value.

The current API (`include/anyfs.h`) wraps only two things:

1. **Kernel lifecycle** — `anyfs_kernel_init()` / `anyfs_kernel_halt()` hide the
   `lkl_init()` + `lkl_start_kernel()` argument plumbing.
2. **Multi-backend disk registration** — `anyfs_disk_add()` / `anyfs_disk_remove()`
   select `raw`/`gio`/`qemu` from the flags and call `lkl_disk_add()`.

`include/anyfs_disk.h` adds a session layer on top (`anyfs_disk_open()`,
`anyfs_disk_list()`, `anyfs_disk_enter_path()`, …) so frontends (FUSE, ksmbd, nfsd,
WinFSP) share one canonical way to list and enter partitions and nested containers.

For everything else the consumer uses LKL syscalls directly — full Linux VFS, no
feature gates.

---

## 3. I/O Path

LKL's virtio I/O path completes synchronously on the calling host thread:

```
lkl_sys_read()
  → VFS → submit_bio()
  → virtqueue_notify → writel(QUEUE_NOTIFY)
  → iomem_access → virtio_process_queue
  → ops->request(disk, &req)     ← backend runs here
  → virtio_req_complete → lkl_trigger_irq
  → writel returns → bio completes
```

Each backend's `request()` implementation:

- **raw**: `pread(fd, buf, len, offset)`
- **gio**: `g_seekable_seek()` + `g_input_stream_read()`
- **qemu**: `blk_pread(blk, offset, len, buf, 0)` (uses QEMU coroutines internally)

---

## 4. Block-Backend Interface

```c
/* LKL's block device interface (lkl_host.h) */
struct lkl_dev_blk_ops {
    int (*get_capacity)(struct lkl_disk disk, unsigned long long *res);
    int (*request)(struct lkl_disk disk, struct lkl_blk_req *req);
};

/* anyfs backend abstraction (src/core/anyfs_backend.h) */
struct anyfs_backend_ops {
    const char *name;
    int  (*open)(const char *path, int readonly, struct lkl_disk *disk_out);
    void (*close)(struct lkl_disk *disk);
};
```

`anyfs_disk_add()` internals:

1. Pick `anyfs_backend_ops` from the flags (default QEMU > raw).
2. Call `ops->open()` to fill an `lkl_disk` (carries `lkl_dev_blk_ops`).
3. Call `lkl_disk_add()` to register it with the LKL kernel.

---

## 5. QEMU Block-Backend Details

### 5.1 Symbol Isolation

- `libanyfs_core.a` is compiled with `-fvisibility=hidden`.
- The thousands of QEMU internal symbols stay hidden in the shared/static output.

### 5.2 AioContext Per-Thread Problem

LKL may call `ops->request()` from any host thread. QEMU's `blk_pread` requires the
calling thread to have an AioContext bound. The backend installs one lazily:

```c
static __thread bool aio_ctx_set = false;
if (!aio_ctx_set) {
    qemu_set_current_aio_context(qemu_get_aio_context());
    aio_ctx_set = true;
}
```

### 5.3 Format-Driver Registration

QEMU format drivers register via `block_init()` (a `__attribute__((constructor))`
hook). The link line therefore needs `-Wl,--whole-archive libblock.a -Wl,--no-whole-archive`.

### 5.4 Building QEMU

```bash
cd ~/qemu && mkdir -p build-anyfs-shared && cd build-anyfs-shared
../configure --disable-system --disable-user --enable-tools \
    --disable-guest-agent --disable-docs --disable-gtk --disable-sdl \
    --disable-iscsi --disable-nfs --disable-ssh --disable-curl \
    --target-list= \
    --extra-cflags="-fPIC"
meson configure -Db_pie=false
ninja libqemuutil.a libqom.a libauthz.a libcrypto.a libio.a \
      libblock.a libevent-loop-base.a
```

Scripts: `scripts/build_qemu.sh` drives the multi-target (linux-amd64 / mingw32 /
mingw64) build and produces `libanyfs-qemublk.{so,dll}`.

---

## 6. Performance

Detailed benchmark results: see [README.md](../README.md#benchmarks).

Summary: raw ≈ 1.3 GB/s, gio ≈ 1.1 GB/s, qemu ≈ 0.8 GB/s (~150 files / ~150 MiB ext4).
Single 4 KiB pread latency: raw ≈ 29 µs, gio ≈ 34 µs.

---

## 7. Why Async Was Rejected

Three approaches were prototyped and abandoned:

1. **GIO async backend** — thread-bridge overhead adds ~15 µs/op, no benefit for
   synchronous LKL.
2. **Linux AIO (`io_submit`) without a worker thread** — `O_DIRECT` bypasses the
   host page cache; `mount` runs 6× slower.
3. **`qemu_blk_aio_preadv`** — `lkl_trigger_irq` from an external thread cannot
   wake the LKL idle loop.

Root cause: LKL's `posix_idle` blocks in `poll()`; external threads that flip the IRQ
pending bit have no way to wake that `poll()`. Patching the LKL idle loop is
invasive and brittle.

Conclusion: synchronous I/O is sufficient for almost all workloads (29 µs/op ≈
34 k IOPS per LKL CPU).

---

## 8. Host-Side Proxy (ksmbd / nfsd Data Path)

`anyfs-ksmbd` and `anyfs-nfsd` bind their in-LKL listeners to `127.0.0.1:445`/`:2049`
inside the LKL kernel. Instead of routing client traffic through libslirp NAT, a
host-side userspace proxy (`src/host_proxy/`) accepts plain TCP on the host port
and spawns one LKL socket per accepted connection, copying bytes between the two
with a 2-thread + SPSC + epoll loop. libslirp is therefore **not** on the data path.

See [lkl-servers.md](lkl-servers.md) for the host_proxy design and
[anyfs-fuse.md](anyfs-fuse.md) for the FUSE frontend.

---

## 9. Frontends Shipped Today

| Binary             | Target              | Description                                       | Meson option        |
| ------------------ | ------------------- | ------------------------------------------------- | ------------------- |
| `anyfs-ksmbd`      | Linux / mingw       | SMB3 server (in-LKL `ksmbd` + ksmbd-tools IPC)    | `-Denable_ksmbd`    |
| `anyfs-nfsd`      | Linux / mingw       | NFSv4 server (in-LKL `nfsd` + mini mountd)        | `-Denable_ksmbd` (reuses LKL build) |
| `anyfs-fuse`       | Linux               | FUSE3 frontend                                    | `-Denable_fuse`     |
| `anyfs-winfsp`     | mingw32             | WinFSP frontend                                   | `-Denable_winfsp`   |
| `anyfs-lspart`     | Linux / mingw       | List partitions / probe kind                      | always built        |
| `bench_backends`   | Linux               | Per-backend mount + readall benchmark             | always built        |
| `test_raw_mount`   | Linux               | Regression: mount a partition and read a file     | always built        |
| `lkl-zerobench`    | Linux               | In-LKL zero-copy bisection harness                | always built        |
| `lkl-loopbench`    | Linux               | Userspace TCP loopback benchmark                  | always built        |
| `lkl-shmem-relay-bench` | Linux          | kthread + shared-ring relay benchmark             | always built        |

Out of scope (removed): the old `anyfs-shell` CLI and `anyfs-gui` GTK3 file manager;
the 7-Zip plugin. Their docs have been removed.

---

## 10. Roadmap

| Phase | Item                                                       | Status |
| ----- | ---------------------------------------------------------- | :----: |
| PoC   | LKL 6.12 build, raw mount validation                       | ✅     |
| 2     | anyfs API + raw backend                                    | ✅     |
| 2a    | GIO synchronous backend                                    | ✅     |
| 3     | QEMU block backend (qcow2/vmdk/vdi)                        | ✅     |
| 5     | API slimming (`anyfs.h` 4 functions)                       | ✅     |
| 7     | ksmbd SMB3 server                                          | ✅     |
| 8     | nfsd NFSv4 server (pynfs 98.2% pass)                       | ✅     |
| 9     | Windows port (mingw, LKL 6.18, ksmbd via wine)             | ✅     |
| 10    | FUSE frontend (`anyfs-fuse`)                                | ✅     |
| 11    | WinFSP frontend (`anyfs-winfsp`)                           | ✅     |
