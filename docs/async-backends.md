# AnyFS-Reader: True Async Disk Backends

## Overview

AnyFS-Reader uses LKL (Linux Kernel Library) to mount and read filesystem images
in userspace. This document describes the **true async disk backend** architecture
introduced in Phase 2d.

## Backend Comparison

| Backend | File | Mechanism | Async? | Best For |
|---------|------|-----------|--------|----------|
| `raw` | `raw_blk_backend.c` | `pread()` inline | No | Local fast storage |
| `aio` | `aio_blk_backend.c` | Linux AIO + eventfd + epoll | **Yes** | O_DIRECT, high-latency I/O |
| `gio-sync` | `gio_blk_backend.c` | GIO `g_input_stream_read_all()` | No | Cross-platform (GLib) |
| `gio-async` | `gio_async_blk_backend.c` | GIO on dedicated GMainLoop thread | **Yes** | Cross-platform async |

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  LKL Kernel (ext4/xfs/btrfs/vfat)                   │
│                                                     │
│  virtio-blk driver → virtio_process_queue()         │
│       │                                             │
│       ▼                                             │
│  blk_enqueue(req) → backend.request(disk, blk_req) │
│       │                                             │
│       │  returns LKL_DEV_BLK_STATUS_PENDING (255)   │
│       ▼                                             │
│  virtio_process_one: saves _req on heap,            │
│  advances last_avail_idx, returns 0                 │
│       │                                             │
│  LKL thread yields → scheduler runs other threads   │
└─────────────────────────────────────────────────────┘
         │
         │ (async completion from I/O thread)
         ▼
┌─────────────────────────────────────────────────────┐
│  Host I/O Thread                                    │
│                                                     │
│  [AIO path]                                         │
│    io_submit(iocb) → kernel AIO → eventfd           │
│    reaper: epoll_wait → io_getevents → complete     │
│                                                     │
│  [GIO-async path]                                   │
│    idle source → g_input_stream_read_all → complete  │
│                                                     │
│  Completion:                                        │
│    *(uint8_t*)status_ptr = OK/IOERR                 │
│    lkl_disk_complete_req(opaque)                    │
│       → virtio_req_complete(_req)                   │
│       → lkl_trigger_irq(dev->irq)                  │
│       → wakes waiting LKL thread                    │
└─────────────────────────────────────────────────────┘
```

## LKL Patches Required

The following patches to `linux/tools/lkl/` enable true async:

### 1. `include/lkl_host.h`
- Added `void *opaque` and `void *status_ptr` fields to `struct lkl_blk_req`
- Added `#define LKL_DEV_BLK_STATUS_PENDING 255`

### 2. `include/lkl.h`
- Declared `void lkl_disk_complete_req(void *opaque)`

### 3. `lib/virtio_blk.c`
- `blk_enqueue()`: sets `lkl_req.opaque = req` and `lkl_req.status_ptr = &t->status`
- If status == PENDING after request(), returns 0 (not error)
- New `lkl_disk_complete_req()`: calls `virtio_req_complete(opaque, 0)`

### 4. `lib/virtio.c`
- `virtio_process_one()`: allocates `_req` on heap (`lkl_host_ops.mem_alloc`)
- Advances `q->last_avail_idx` BEFORE calling `enqueue` (critical for async correctness)
- `virtio_req_complete()`: frees `_req` with `lkl_host_ops.mem_free`

### Key Correctness Invariants
1. `blk_enqueue` returns 0 for PENDING (not -EINPROGRESS) — otherwise `virtio_process_queue` breaks the loop
2. `last_avail_idx` must advance before async return — otherwise the same descriptor is re-processed
3. `_req` is freed ONLY in `virtio_req_complete` — it persists across the async gap

## AIO Backend (`aio_blk_backend.c`)

**Design**: Linux native AIO (io_submit/io_getevents syscalls, NOT libaio) with
eventfd notification and epoll-based reaper thread.

```
request() thread:                    Reaper thread:
  alloc bounce buffer (512-aligned)    epoll_wait(epoll_fd)
  setup iocb (IOCB_CMD_PREAD)         read(eventfd) → consume counter
  iocb.aio_flags = IOCB_FLAG_RESFD    io_getevents() → harvest completions
  iocb.aio_resfd = eventfd            for each event:
  io_submit() → returns immediately     memcpy bounce → user buf (if read)
  return LKL_DEV_BLK_STATUS_PENDING     set *status_ptr = OK/IOERR
                                         lkl_disk_complete_req(opaque)
                                         free_aio_req()
```

- Uses O_DIRECT for true async (bypasses page cache)
- Pool of 256 `struct aio_req` (avoids malloc per I/O)
- Falls back to synchronous `pread` if `io_submit` fails

## GIO-Async Backend (`gio_async_blk_backend.c`)

**Design**: Dedicated I/O thread with private GMainContext/GMainLoop.
Requests are dispatched as idle sources; reads are synchronous on the I/O thread.

```
request() thread (LKL):              I/O thread (GMainLoop):
  alloc AsyncReq                       g_main_loop_run(io_loop)
  attach idle source to io_context     dispatch_read_idle():
  return LKL_DEV_BLK_STATUS_PENDING     g_mutex_lock(stream_lock)
                                         for each iov:
                                           g_seekable_seek()
                                           g_input_stream_read_all()
                                         g_mutex_unlock(stream_lock)
                                         set *status_ptr = OK/IOERR
                                         lkl_disk_complete_req(opaque)
                                         g_free(ar)
```

- Stream serialized via `stream_lock` (single GFileInputStream)
- True async from LKL's perspective (LKL thread freed during I/O)
- No O_DIRECT requirement (works on any platform with GLib)

## Benchmark Results

### Fast Storage (Local SSD, page cache warm)

```
4 threads × 200 reads:
  raw          | 0.043 ms/op | 91.1 MB/s
  aio          | 0.048 ms/op | 82.2 MB/s
  gio-sync     | 0.051 ms/op | 77.3 MB/s
  gio-async    | 0.042 ms/op | 92.6 MB/s  ← slightly wins (less lock contention)
```

On fast storage: all backends perform similarly. Async overhead is negligible.

### High-Latency Storage (dm-delay, 50ms per I/O)

```
4 threads × 20 reads (50ms delay):
  raw          | 3.53 ms/op | 1.1 MB/s  (baseline)
  aio          | 2.19 ms/op | 1.8 MB/s  (+63% throughput)
  gio-sync     | 3.58 ms/op | 1.1 MB/s  (same as raw)
  gio-async    | 2.84 ms/op | 1.4 MB/s  (+27% throughput)

8 threads × 20 reads (50ms delay):
  raw          | 1.78 ms/op | 2.2 MB/s  (baseline)
  aio          | 1.10 ms/op | 3.6 MB/s  (+64% throughput)
  gio-sync     | 1.77 ms/op | 2.2 MB/s  (same as raw)
  gio-async    | 1.44 ms/op | 2.7 MB/s  (+23% throughput)
```

On high-latency storage: async pipelining provides **23-64% throughput improvement**
by overlapping multiple I/Os during the latency window.

## Usage

```c
#include "anyfs_api.h"

// Select backend via flags
anyfs_open_image(ctx, "/dev/sda1", ANYFS_OPEN_READONLY | ANYFS_OPEN_AIO);
anyfs_open_image(ctx, "disk.img", ANYFS_OPEN_READONLY | ANYFS_OPEN_GIO_ASYNC);
```

### Running Benchmarks

```bash
# Sequential benchmark (all backends, forked)
./builddir/bench_backends /tmp/test.img ext4 0 1000

# Concurrent benchmark (multi-threaded)
./builddir/bench_concurrent /tmp/test.img ext4 0 8 200

# With dm-delay for high-latency simulation:
sudo losetup /dev/loop0 /tmp/test.img
SECTORS=$(sudo blockdev --getsz /dev/loop0)
echo "0 $SECTORS delay /dev/loop0 0 50" | sudo dmsetup create slow_disk
sudo chmod 666 /dev/mapper/slow_disk
./builddir/bench_concurrent /dev/mapper/slow_disk ext4 0 8 20
sudo dmsetup remove slow_disk && sudo losetup -d /dev/loop0
```

## File Layout

```
src/core/
  anyfs_core.c              # Main API: init, open_image, mount, file ops
  raw_blk_backend.c         # Sync pread backend
  raw_blk_backend.h
  aio_blk_backend.c         # True async: Linux AIO + eventfd + epoll
  aio_blk_backend.h
  gio_blk_backend.c         # GIO sync backend
  gio_blk_backend.h
  gio_async_blk_backend.c   # True async: GMainLoop I/O thread
  gio_async_blk_backend.h

tests/
  test_raw_mount.c          # Functional test (all backends)
  run_tests.sh              # Full test suite (4 FS × 2 layouts × 4 backends = 32)
  bench_backends.c          # Sequential benchmark
  bench_concurrent.c        # Concurrent multi-threaded benchmark

include/
  anyfs_api.h               # Public API header
```
