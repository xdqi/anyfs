# QEMU dedicated-thread embedding design

**Date:** 2026-10-05
**Status:** implemented (phases 1–4, 2026-10-05), verified on real Windows. The wasm small-op
performance gap and the wasm atomics issue are open; see "Implementation notes" at the end.
**Scope:** `src/core/qemu_backend.c` (rewrite), `src/core/qemu_thread.{c,h}` (new),
`patches/qemu/` (two new patches), `ts/native/anyfs_ts.c` + `ts/packages/core/src/worker.ts`
(wasm API thread), `ts/packages/anyfs-native/src/binding.cc` (native addon)

## Goal

Make the QEMU block layer (the image-format layer: qcow2, vmdk, vhdx, vdi, vhd, dmg, NBD,
curl) stable in every anyfs edition by changing **how it is embedded**, not whether it is
used. QEMU stays; the direction is wasm-first for safety, with native as the faster
alternative, so the embedding must be sound in both.

Today anyfs calls QEMU as a library from other parties' threads and event loops. QEMU's
block layer assumes it owns its AioContext and runs it from one home thread. Every
QEMU-related defect class we have hit traces back to that mismatch. After this change,
each process (or wasm instance) runs QEMU on one dedicated thread that owns QEMU's event
loop; everyone else hands it requests and waits.

## Context — evidence for the root cause

Editions in scope:

| Edition | Where QEMU runs today | Current problem |
|---|---|---|
| Web / Electron-wasm | inside the wasm instance, entered from JS `ccall` and from LKL kernel threads | Asyncify patch chain; whole-module Asyncify instrumentation |
| Electron-native | N-API addon in the Electron main process | F7 (crash at mount, worked around by a synchronous enter), F9 (hang on close / file switch); open and enter block the UI thread |
| Native CLI (lspart, anyfs-ksmbd, nfsd, FUSE) | plain process, no foreign loop | works, but uses a different embedding from the other editions |

Findings:

- **QEMU's main AioContext is attached to the global default GLib context.**
  `qemu_init_main_loop` (`~/qemu/util/main-loop.c:179-185`) calls
  `g_source_attach(src, NULL)` for both the aio-context and io-handler sources. On Linux,
  Electron's main thread iterates `g_main_context_default()`, so Chromium dispatches QEMU's
  AioContext while QEMU also drives it from `aio_poll`. This is F7's gdb-confirmed root
  cause (`fdmon-io_uring.c get_sqe` assertion; epoll fdmon segfaults instead)
  (`ts/tests/e2e/FINDINGS.md` F7).
- **Every LKL thread impersonates QEMU's main thread.** `qemu_request`
  (`src/core/qemu_backend.c:71-77`) sets QEMU's main AioContext as the current context of
  whichever LKL thread calls it, then runs the synchronous `blk_pread`, which spins
  `aio_poll` internally. At least three parties touch one AioContext: open/close on the
  caller thread, reads on LKL threads, and (on Linux Electron) the Chromium GLib loop.
- **Lifecycle calls are pinned to the Electron main thread.** `blk_new_open` / `blk_unref`
  assert `qemu_in_main_thread()`, so `kernelInit` / `sessionOpen` / `sessionClose` are
  synchronous on the Electron main thread (`binding.cc:260-264`), and `sessionEnter` was
  reverted to synchronous as the F7 workaround (`binding.cc:336-345`). A slow URL image
  open therefore blocks the Electron UI thread.
- **F9 is confirmed on real Windows**, where Chromium does not use GLib, so the GLib
  entanglement cannot be its whole cause there. Hypothesis (unverified): teardown waits on an
  AioContext that no thread is polling.
- **wasm: coroutine switches unwind the LKL kernel stack.** The bundle is linked with
  `-sASYNCIFY=1` and no `ASYNCIFY_ONLY` / `ASYNCIFY_REMOVE` lists
  (`scripts/build_anyfs_wasm.sh:259`), so the whole 68 MB module, LKL included, is
  instrumented. A QEMU coroutine switch (emscripten fiber swap) unwinds the entire stack of
  the calling thread; block reads are issued from deep inside virtio-blk and filesystem
  code, so each read unwinds and rewinds kernel frames. `ASYNCIFY_STACK_SIZE=131072`
  (`build_anyfs_wasm.sh:260`) is consistent with that.
- **wasm: the glue layer works around Asyncify on every call.** Fiber swaps discard export
  return values, so every glue entry point has a `_p` out-pointer variant and i64 offsets
  are split into halves (`ts/native/anyfs_ts.c:502-560`); every `ccall` must pass
  `{async:true}` (`worker.ts:84-106`). QEMU-side workarounds live in `patches/qemu/`:
  `0001` (inline thread pool), `0004` (poll as a 1 ms sleep loop), `0005` (fdmon-poll
  thread-locals), `0006` (stack allocation without mprotect).
- **Upstream QEMU already supports emscripten.** `~/qemu` is v11.0.0 with
  `util/coroutine-wasm.c`; the anyfs runtime patches exist because of the embedding mode,
  not because QEMU lacks a wasm port.
- **The last-error buffer is thread-local** (`static __thread char g_last_error[512]`,
  `src/core/anyfs_kernel.c:24`). An error set on another thread is invisible to the caller.
- `qemu_backend.c` is the only code that calls QEMU directly; `binding.cc` only references
  QEMU in comments. `src/fuse/fuse_main.c:1474` selects the backend by flag only.

## Decisions (from design review)

| Question | Decision |
|---|---|
| Keep QEMU as the image layer? | Yes. Change the embedding only. (libyal readers are LGPL-3.0 and cannot be linked with GPL-2.0-only LKL; a self-written reader set was rejected.) |
| Embedding approach | **A — dedicated QEMU thread**, in-process, same design in every edition |
| Rejected: separate image service (qemu-storage-daemon + NBD; second wasm module + SAB ring) | Strongest isolation, but costs the native CLI an extra process and an NBD hop per request, and doubles the transports to maintain |
| Deferred: Electron `utilityProcess` (option C) | Compatible with A; add later only if Electron-native needs crash isolation, or if F9 survives A on Windows |
| Hang handling | Detect and report fatal; never recover in-process |
| Legacy inline embedding | Kept behind a compile-time switch only until the wasm phase lands, then deleted |

## Invariants

The design is defined by these; tests verify them.

1. **Single owner.** Only the QEMU thread calls QEMU functions: QOM/bdrv init, main-loop
   init, option QDict building, open, read, write, flush, close.
2. **No foreign loop.** QEMU's AioContext and io-handler GSources are attached to a private
   `GMainContext`, never the global default context.
3. **Waiters only wait.** Callers block on a semaphore/futex; they never run QEMU's loop.
4. **(wasm) Asyncify unwinds happen only on the QEMU thread's stack**, never through LKL
   kernel frames.
5. **(wasm, existing constraint, preserved) The module-owning worker never blocks.**
   WORKERFS/URLFS file reads issued by pthreads are proxied to it; if it blocks waiting for
   QEMU, the QEMU thread's file read can never complete.
6. **No re-entry.** The QEMU thread never calls `lkl_*`, never waits on LKL, and never calls
   `qemu_thread_call` / `qemu_thread_co_call` (asserted).

## Architecture

```
            caller threads                                   QEMU thread (one per process / wasm instance)
 ┌──────────────────────────────────┐                     ┌─────────────────────────────────────────────┐
 │ LKL kernel threads (block I/O)   │  request + wait     │ private GMainContext                         │
 │ wasm API pthread (session ops)   │ ──────────────────▶ │ QEMU main AioContext (home thread = this)    │
 │ libuv pool (Electron AsyncWorker)│  aio_bh_schedule_   │ loop: aio_poll(ctx, true)                    │
 │ CLI main thread                  │  oneshot + sem      │   BH → open/close, or coroutine → blk_co_*   │
 └──────────────────────────────────┘ ◀────────────────── └─────────────────────────────────────────────┘
                                        result + error string
```

### Components

**1. `src/core/qemu_thread.{c,h}` (new)** — lifecycle of the QEMU thread. Public surface:

```c
/* Start the QEMU thread if not running; wait for it to finish init.
 * Idempotent and thread-safe. Returns 0, or negative with an error string. */
int qemu_thread_start(char *err, size_t err_cap);

/* Run fn(opaque) on the QEMU thread as a bottom half; block until it returns.
 * For open/close (global-state code). */
int qemu_thread_call(void (*fn)(void *opaque), void *opaque);

/* Run fn(opaque) on the QEMU thread as a coroutine; block until it returns.
 * For read/write/flush (blk_co_* I/O code). No nested aio_poll in a BH. */
int qemu_thread_co_call(CoroutineEntry *fn, void *opaque);

/* Ask the loop to exit and join the thread. Later calls fail fast. */
void qemu_thread_stop(void);

/* True on the QEMU thread (for assertions). */
bool qemu_thread_is_current(void);
```

- Uses QEMU's portable `QemuThread` / `QemuSemaphore` (Linux, mingw, emscripten pthreads).
- On start, the thread creates a private `GMainContext`, registers it with the P1 hook,
  runs `module_call_init(MODULE_INIT_QOM)`, `bdrv_init()`, `qemu_init_main_loop()`, posts
  "ready", then loops on `aio_poll(qemu_get_aio_context(), true)` until a stop flag is set
  by a stop BH. A side effect: `qemu_signal_init`'s `pthread_sigmask` now applies to the
  QEMU thread instead of whichever thread opened the first image (today possibly the
  Electron main thread).
- Lazy start on the first QEMU-backed open, matching today's `qemu_initialized`.
- Waits use a timeout (see Error handling).

**2. `src/core/qemu_backend.c` (rewrite)** — `open`, `close`, `request` become thin
marshalling wrappers around a request struct that carries inputs, outputs and an error
string. Removed: the per-thread AioContext impersonation (lines 71-77) and the wasm
`emscripten_sleep(0)` "force Asyncify" call in open. NBD (`nbd-fd:` / `nbd-port:`) and URL
option QDicts are built inside the QEMU-thread open function. The wasm `/var/tmp` `mkdir`
for snapshot overlays stays (plain libc). The `anyfs_backend_ops` interface is unchanged.

**3. QEMU patches (`patches/qemu/`)**

- **P1 (all platforms): embedder GLib context.** Add
  `void qemu_main_loop_set_gcontext(GMainContext *ctx)` to `util/main-loop.c` (declared in
  `include/qemu/main-loop.h`). `qemu_init_main_loop` passes the stored context to both
  `g_source_attach` calls; unset means `NULL`, i.e. unchanged upstream behaviour.
- **P2 (emscripten only): futex idle wait, replacing `0004`.** On a dedicated thread,
  `0004`'s 1 ms sleep loop adds up to 1 ms of latency to every block request.
  P2 makes `qemu_poll_ns` do a zero-timeout `g_poll`; if nothing is ready, it
  `emscripten_futex_wait`s on a wake-up word with the caller's timeout (QEMU's timer
  deadline). The emscripten `EventNotifier` set path bumps the word and
  `emscripten_futex_wake`s. This removes both the latency floor and the need for Asyncify
  sleeps in poll.
- `0001`, `0005`, `0006` and the build patches stay. Revisit them after phase 3.

**4. wasm glue (`ts/native/anyfs_ts.c`, `ts/packages/core/src/worker.ts`)** — a persistent
**API pthread** runs every `anyfs_ts_*` operation. The module-owning worker only dispatches
an operation (args in linear memory) and asynchronously awaits a completion notification
(proxied back to the module-owning thread). This generalizes the existing boot/enter
async pattern (`anyfs_ts.c:145-227`) to all ops and is what upholds invariant 5 once
waiting for QEMU becomes a real blocking wait. Afterwards the `_p` variants, the i64
lo/hi split and `{async:true}` are retired.

**5. Native addon (`ts/packages/anyfs-native/src/binding.cc`)** — `kernelInit`,
`sessionOpen`, `sessionClose` and `sessionEnter` become `AsyncWorker`s like the other ops;
the F7 synchronous-enter workaround and its comment are removed. `g_op_mutex` stays (LKL
has one CPU; serial ops are sufficient). A fatal hook (below) is wired to a
`Napi::ThreadSafeFunction` that `NativeSession` turns into `onFatal`.

**6. Native CLI (lspart, anyfs-ksmbd, nfsd, FUSE)** — no source changes expected; they pick
up the new embedding through `libanyfs_core`.

**7. Compile-time switch `ANYFS_QEMU_THREAD`** — selects the new embedding. Phase 1: on for
native (meson), off for wasm (`build_anyfs_wasm.sh`), which keeps the legacy inline path
until the wasm glue is ready. Phase 3 turns it on for wasm; once phase 3 is green, the
legacy path and the switch are deleted.

## Data flow

**Open** (`anyfs_disk_add` → `qemu_blk_open`), on the caller thread:
1. `qemu_thread_start()`; on failure, set last-error from the returned string.
2. Fill `{path, flags, out blk, out capacity, err[256]}`; `qemu_thread_call(open_fn)`.
3. QEMU thread: build the QDict, `blk_new_open`, `blk_getlength`; on failure, write `err`
   and unref; post the semaphore.
4. Caller: on failure, `anyfs_set_last_error(err)` on its own thread; on success,
   `lkl_disk_add`. The partition scan that follows uses the read path.

**Read / write / flush** (`qemu_request`), on an LKL kernel thread holding the LKL CPU:
1. Fill `{blk, type, offset, iov[], count, ret}`; `qemu_thread_co_call(io_co)`.
2. QEMU thread: a BH creates a coroutine that walks the iov with `blk_co_preadv` /
   `blk_co_pwritev` / `blk_co_flush`, stores `ret`, posts the semaphore. In wasm, file
   reads from `/work/<name>` are proxied to the module-owning worker, which is idle by
   invariant 5.
3. LKL thread wakes and returns the status.

**Close** (`anyfs_disk_remove`): filesystems are unmounted and `lkl_disk_remove` runs first,
so LKL issues no new requests; then `qemu_thread_call(close_fn)` runs `blk_unref` on the
QEMU thread. Draining happens on the thread that is always polling, which is the
expected fix for F9's teardown stall (to be verified per platform).

**Kernel halt / process exit**: close all sessions → LKL halt → `qemu_thread_stop()`. The
atexit disk drain follows the same order. Any call after stop returns an error immediately.

## Error handling

| Condition | Handling |
|---|---|
| Open fails | Error string travels back in the request; the QEMU thread keeps serving other disks |
| QEMU thread fails to start | That open fails with the reason; the failure is sticky and later opens report it |
| QEMU thread crashes (abort, assertion, segfault) | Not catchable in-process. wasm: the runtime aborts → existing worker `abort` event → `onFatal` → UI error → user reopens. Native CLI: process exits. Electron-native: main process dies (isolation is option C, deferred) |
| QEMU thread hangs (corrupt image loop, stalled network) | Watchdog, below |

**Watchdog: detect, report, do not recover.**
- Every wait (`qemu_thread_start` readiness, `qemu_thread_call`, `qemu_thread_co_call`)
  uses `qemu_sem_timedwait` with a single default of **120 s**. It is a hang detector,
  not a latency target. Native builds can override it with the environment variable
  `ANYFS_QEMU_TIMEOUT_MS`. Per-source tuning is deferred until there is evidence it is
  needed.
- On timeout the waiter must **not** return EIO to LKL: the QEMU coroutine may still write
  into the iov, which is LKL page memory, and an early return would corrupt memory later.
- Instead the waiter calls a fatal hook once, `anyfs_fatal(const char *reason)`, then keeps
  waiting. Hooks are installed per edition via `anyfs_set_fatal_hook()`:
  - wasm glue: proxies an async notification to the module-owning worker, which emits the
    existing fatal event; the UI disposes the session with the existing
    terminate-first path. Hang → clean error is the sandbox promise.
  - Native addon: `Napi::ThreadSafeFunction` → `NativeSession` fatal → UI suggests wasm
    mode or an app restart.
  - No hook (native CLI): log the reason to stderr and `_exit(1)`. The single LKL CPU is
    blocked behind the request anyway, so a supervisor restart is the only useful action.

Out of scope here: hangs inside LKL filesystem drivers. Those need an API-level watchdog
and belong to the separate corrupt-image robustness project.

## Testing and verification

| Edition / platform | Verification | Pass criteria |
|---|---|---|
| C core (Linux, CI) | Extend `tests/test_qemu_mount.c`: concurrent reads from several threads across several disks; 100 open/close cycles; call-after-stop fails fast; watchdog fires on a stalled `@anyfs/nbd-proxy` source (stall injection) | All green; no leaks; no hangs |
| Native CLI | `tests/bench_backends.c` before vs after (qcow2 sequential + random read); ksmbd/nfsd smoke | Throughput regression ≤ 5% |
| Electron-native (Linux) | Remove the F7/F9 `test.fixme` gates; run the whole `open-browse-download` spec in one worker (the F9 repro); `switch.spec.ts` | Green; `app.close()` completes |
| Electron-native (Windows) | wine first (QEMU thread on mingw), then real Windows: open qcow2 → switch files 5× → quit | No hang. If real Windows still hangs, F9 has a Windows-specific cause → add option C |
| Web / Electron-wasm | All six E2E flows (formats covers qcow2/vmdk/vhdx/vdi/vhd/dmg); fixed perf workload: list partitions → mount → walk N files → read a 256 MiB file, from a local qcow2 and from a URL | Functionally green; not slower than before |

Debug builds also assert invariants 1 and 6 (`qemu_thread_is_current()` checks in the
backend and in `qemu_thread_*`).

## Phasing

Each phase lands on `main` on its own; `main` stays green throughout.

- **Phase 0 — spike.** Answer three questions:
  1. wasm: can QEMU run on a dedicated pthread with P2's futex idle wait alongside
     emscripten fibers, and what is the per-request latency?
  2. Linux Electron: with P1 and `sessionEnter` back on an `AsyncWorker`, is F7 gone?
  3. Windows: does F9 disappear?

  Proceed if 1 and 2 are yes. If 3 is no, A still proceeds (it fixes the rest) and option C
  is scheduled for Windows.
- **Phase 1 — C core.** `qemu_thread`, `qemu_backend.c` rewrite, P1, fatal hook,
  `ANYFS_QEMU_THREAD` (native on, wasm off). C tests and native CLI benchmarks green.
- **Phase 2 — native addon.** All ops as `AsyncWorker`; ThreadSafeFunction fatal hook;
  electron-native E2E un-gated; Windows verification.
- **Phase 3 — wasm.** P2, API pthread, wasm fatal hook, switch on for wasm; retire `_p`,
  i64 split and `{async:true}`; web + electron-wasm E2E green; perf comparison. Then delete
  the legacy inline path and the switch.
- **Phase 4 — optional, measurement-gated.** Restrict Asyncify instrumentation to QEMU code
  (`ASYNCIFY_ONLY` / `ASYNCIFY_REMOVE`). Keep it only if E2E stays green and size or speed
  improves measurably.
- **Wrap-up.** Update FINDINGS F7/F9 status and `docs/open-image-flow.md`; record memory.

## Out of scope

- Replacing QEMU or adding a second image-format implementation.
- Making LKL block requests complete asynchronously. LKL's virtio-blk appears to process
  requests synchronously in the notifying kernel thread (`tools/lkl/lib/virtio.c:474`),
  which stalls the single LKL CPU for the duration of each QEMU request; the request queue
  introduced here would make that change possible later.
- Electron `utilityProcess` isolation (option C), unless phase 0 or 2 shows it is needed
  for Windows.
- LKL-side hang detection (API-level watchdog) and the corrupt-image test corpus.
- Custom mount options, ZFS pool import, LUKS/LVM.

## Acceptance criteria

1. In every edition, QEMU functions run only on the QEMU thread (debug assertions hold
   across the full test matrix).
2. Electron-native on Linux: the F7 crash and F9 hang no longer reproduce, and open/enter
   no longer block the Electron main thread.
3. Electron-native on Windows: F9 is verified fixed, or option C is scheduled with evidence.
4. wasm: all E2E flows green with the API thread; `_p` variants, the i64 split and
   `{async:true}` are gone; the perf workload is not slower than before.
5. Native CLI throughput regresses by no more than 5%.
6. A stalled image source produces a fatal error (wasm/Electron) or a clean exit (CLI)
   within the watchdog timeout, never a silent hang.
7. The legacy inline embedding and the `ANYFS_QEMU_THREAD` switch are removed.

## Implementation notes (2026-10-05)

Phases 1–3 landed as designed, with these differences:

- **P2 is required for correctness, not just latency.** emscripten 5.0.7 marks `poll()` and `fsync()`
  as async imports. When a pthread proxies either one to the main thread, the main thread runs it
  through Asyncify and aborts ("… was not in ASYNCIFY_IMPORTS, but changed the state"). Once QEMU
  left the main thread, every `poll()` call hit this, even with a zero timeout. Patch
  `0004-emscripten-avoid-async-syscalls.patch` replaces the old sleep loop:
  - `qemu_poll_ns` waits on a futex that `event_notifier_set` bumps, then reports every fd ready.
    This is safe because, in this build, the only fds are EventNotifier pipes, drained with
    non-blocking reads.
  - `qemu_fdatasync` calls the synchronous `fdatasync()`.
- **No join on shutdown.** A pthread whose stack Asyncify has unwound for a coroutine switch never
  reaches the thread-exit path that `pthread_join` waits for. The QEMU thread is therefore detached:
  `qemu_thread_stop()` waits, under the watchdog, on a semaphore the thread posts after leaving its
  loop.
- **The API thread serves Node too.** NodeWasmSession and `bootModule` run the bundle on Node's main
  thread, which serves NODEFS calls exactly as the browser Worker serves WORKERFS calls. All callers,
  including the bundle smoke test, use the shared `wasm-api.ts` protocol.
- **Bugs surfaced and fixed along the way:**
  - FINDINGS F17: reopening a disk in one kernel listed no partitions.
  - FINDINGS F18: addon errors threw from inside the AsyncWorker callback instead of rejecting.
  - `build_qemu.sh` reported success when a target failed.
  - The wasm export generator exported a struct type name.
- **Native performance:** the hand-off first cost about 28 µs per block request on a Hyper-V guest,
  5–10% throughput on a small-request workload (88k requests averaging 11.6 KB). The fix came from
  counting wake-ups rather than tuning the hand-off (adaptive polling on the QEMU thread did not
  help). LKL has one CPU, so at most one request is ever in flight, yet QEMU still sent each file
  `preadv` and each qcow2 decompression to its worker pool, at two cross-thread wake-ups per hop.
  Patch `0011-thread-pool-inline-embedder.patch` adds `thread_pool_set_inline()`, which runs
  `thread_pool_submit_co()` work in the calling coroutine. `qemu_thread_fn` enables it on every
  platform. It replaces the emscripten-only `0001` (now in `patches/qemu/retired/`), which inlined
  the same calls at compile time. Reading every file of the trusty qcow2's ext4 partition, three
  alternating rounds in a debug build: legacy inline embedding 32.0 MB/s, QEMU thread before 0011
  29.0 MB/s, with 0011 35.0 MB/s. That clears acceptance criterion 5. On mingw only the qcow2 work
  is inlined: file-win32 uses `thread_pool_submit_aio()`, which the switch leaves alone.
- **Windows (wine):** `bench_backends.exe` read the whole compressed trusty qcow2 through the
  QEMU thread under wine (win32 threads, aio-win32, private GMainContext) and exited cleanly.
- **Shipping a changed QEMU patch:** CI caches the patched `deps/` tree keyed only on
  `peru.yaml`, so editing 0004 in place left CI with the old version applied. The replaced file
  now lives in `patches/qemu/retired/`, and both build scripts reverse-apply retired patches before
  applying the active set.
- **wasm performance (acceptance criterion 4): not met yet.** Workload in Node: open the trusty
  qcow2, list partitions, enter ext4, then walk the tree and read files until 5000 files
  (176 MiB) are done. The baseline is the last pre-refactor bundle (CI run of 3db770f), driven
  with that version's worker.ts call sequence. Its own NodeWasmSession aborted on qcow2
  ("anyfs_ts_session_open is running asynchronously"). Open and enter are as fast or faster
  than before. Walk + read averaged 3042 ms before and 4492 ms now, about 48% slower.

  *Correction:* an earlier version of this note blamed LKL syscalls for being slower on the API
  thread (a cached pread at 57 µs, open at 105 µs). Those figures came from a broken
  microbenchmark: it decoded an empty mount path, so every open failed with ENOENT. With correct
  paths, a cached 4 KiB pread takes 1.5–2.6 µs inside `api_run`, open 40 µs, close 18 µs, lstat
  29 µs.

  A follow-up investigation instrumented the 16k-op walk (about 4.6 s) and found these costs:
  - **API-thread hop, about 50 µs per op (~0.8 s).** That is the dequeue wake, the completion
    through `MAIN_THREAD_ASYNC_EM_ASM`, and the JS continuation. Before, a cached op ran directly
    on the module-owning thread.
  - **Return-address captures (~1.15 s, paid by the old design too, ~1.2 s).** LKL's slab passes
    `_RET_IP_` on every kmalloc and kfree. Emscripten implements `__builtin_return_address` by
    building and parsing a JS stack trace, 10–30 µs per call. Most of open, close and lstat is
    this.
  - **QEMU's syscalls proxied to the module-owning thread (~0.88 s).** Image preads take 62 µs
    each, EventNotifier pipe writes and reads ~51 µs each. In the old design these were direct
    calls on the same thread.
  - **An LKL scheduler hand-off loop, bimodal.** EEVDF's DELAY_DEQUEUE (Linux 6.18) keeps sleeping
    tasks in `rq->nr_running`, so `lkl_cpu_put` sees `!single_task_running()` and cycles host
    thread → idle → `idle_host_task` → host thread, 1–4.5 times per open+close. Turning
    DELAY_DEQUEUE off at runtime removed it.

  Prototype fixes were measured together on the same workload, five alternating rounds. They
  are: DELAY_DEQUEUE off; `emscripten_return_address` stubbed to 0; a bounded synchronous wait
  on the owner thread that keeps serving proxied calls; and short spins on the API and QEMU
  threads. Medians: walk + read 1990 ms, against 2800 ms for the old bundle and 5084 ms for the
  current one. None of these is merged yet. The prototype diff is in the investigation's
  worktree.
- **wasm LKL is compiled without wasm atomics (correctness, open).** The kernel and
  `tools/lkl/lib` objects lack `-matomics`. `__sync_fetch_and_add` on `cpu.shutdown_gate` in
  `lkl_cpu_get`, and `__sync_fetch_and_or` in `set_irq_pending`, compile to plain
  load/modify/store, although several host threads run them. A lost update can drive the gate
  negative, after which every syscall returns -2. That matches intermittent `pread failed: -2`
  runs seen in every bundle, old and new. Whether it also explains the occasional hangs is not
  proven.
- **Phase 4 (Asyncify narrowing): kept.** Only the QEMU thread's stack ever unwinds, when a
  QEMU coroutine switches emscripten fibers. `build_anyfs_wasm.sh` therefore passes every
  function defined in `liblkl.a` (about 37.6k) to `ASYNCIFY_REMOVE`, except names that another
  input also defines. Two link details matter here:
  - The list goes in as `-sASYNCIFY_REMOVE=["@file"]`. With `@file`, emcc expands the names
    onto wasm-opt's command line, which exceeds Linux's 128 KiB per-argument limit.
  - Binaryen's `.`/`#`/`?` substitutions also apply to the list's path, so the path must not
    contain those characters. The script checks this.
  Results on the Node workload, three alternating rounds:
  - the wasm shrinks from 68.2 MB to 32.5 MB;
  - boot + open + list partitions: 3565 → 3116 ms;
  - enter: 93 → 65 ms;
  - walk + read: 4999 → 4420 ms.
  Restricting QEMU and glib as well (`ASYNCIFY_ONLY`) was not attempted. Leaving out one
  function on the unwind path would break at runtime, and the LKL list already captures most
  of the gain.
- **Real Windows (user, 2026-10-05):** the packaged Electron app in native mode no longer hangs
  when switching images or quitting, so F9 is verified fixed. The same session surfaced F20 (hybrid
  ISO, whole disk vs partition), now fixed. Under wine, the pre-refactor addon hung in the same
  probe, and quitting with a disk open hung in both versions (F19, fixed).
- **Not yet done:**
  - wasm performance (above): merge the measured fixes.
  - wasm atomics (above).

