# E2E Suite — Findings (real app behavior surfaced by the tests)

These are **real application defects** the E2E suite surfaced while being built. Per the
project decision, tests that hit these are marked `test.fixme(...)` with a `// FINDING:` comment
rather than having their assertions weakened — the suite stays green while the bugs stay
documented. Each entry: what was observed, how to reproduce, suspected area.

Status legend: OPEN (unconfirmed root cause) · CONFIRMED · FIXED

---

> **Native-vs-wasm spike (Phase 3.2 follow-up):** A diagnostic spike drove the **native** LKL
> addon directly against the same fixtures. Result: **native mounts ext4 and btrfs fine**
> (ext4 → hello.txt/dir/empty/link/lost+found; btrfs-in-a-partition → whole.txt/sub). So
> **F1, F2, F3 were wasm-specific** — the Electron _native_ backend does not hang.
>
> **✅ FIXED (commit 03c1591):** Root cause was a kthread-spawn-while-blocked deadlock in the
> wasm worker — `session_enter` ran synchronously on the Module-owning Worker, which blocked in
> `Atomics.wait`, so the jbd2 (ext4) / worker (btrfs) kthread's `new Worker()` could never be
> created. Fix mirrors the existing boot-async pattern: `anyfs_ts_session_enter_async` runs the
> mount on a dedicated pthread while `worker.ts` polls + yields, keeping the Worker's event loop
> free to spawn the kthread. Verified in-browser: **ext4 now mounts** (lists
> dir/empty/hello.txt/link/lost+found); **F1, F2, F3 resolved.** F5 (below) is separate and
> remains. See memory `project_wasm_mount_kthread_deadlock`.

## F1 — ext4 partition mount hangs in the wasm backend (✅ FIXED 03c1591, was High)

**Observed (Phase 3.2, web/wasm):** Opening `multiRaw` (multi.img), `session.enter(1)` on the
ext4 partition dispatches `op=enter`, EXT4 probes run (`couldn't mount as ext3/ext2` — normal
probe noise), then the worker **never logs `op=enter ... took Xms`** and `enter` never resolves
or rejects. DiskView stays on "mounting partition #1…" indefinitely. Reproduced across multiple
runs.

**Repro:** web app `/?e2e=1` → load multi.img → select partition #1 (ext4) → hangs.

**Suspected area:** wasm LKL `session.enter` / ext4 mount path in the Web Worker (core
`worker.ts` `enter` op, or the LKL ext4 driver under wasm). Contrast: vfat (#2) mounts fine
(`op=enter took 13ms`). **Native mounts ext4 fine** → the bug is in the wasm build/worker, not
the ext4 driver or core session logic.

---

## F2 — whole-disk #0 (no filesystem) hangs instead of erroring [wasm] (✅ FIXED 03c1591 — now errors cleanly, was High)

**Observed:** Entering partition #0 (the whole disk, which has no filesystem on multi.img) —
all FS probes correctly fail to find a filesystem, but instead of surfacing a mount error, the
app **hangs** on "mounting partition #0…"; `enter` never settles.

**Repro:** web app → load multi.img → select whole-disk #0 → hangs (expected: a clean "no
filesystem / can't mount" error).

**Suspected area:** same `enter` path as F1; additionally a missing error path when no FS is
detected (should reject, not hang).

---

## F3 — btrfs mount hangs in the wasm backend (✅ FIXED 03c1591 — no longer hangs; whole-disk btrfs still hits F5, was High)

**Observed:** Opening `btrfsVmdk` (whole-disk btrfs, no PT). dmesg shows `Btrfs loaded`, then
the app is stuck on "mounting…", no rows render, no error. `enter` never settles. Same shape
as F1/F2.

**Repro:** web app → load btrfs-whole.vmdk → enter the whole-disk btrfs → hangs.

**Suspected area:** same wasm `enter`/mount path. Net effect with F1: in this wasm build, only
**vfat** mounts; the directory-bearing filesystems (ext4, btrfs) wedge.

---

## F4 — Service-Worker download fails under `vite preview` (OPEN, Medium; partly env)

**Observed:** `download()` on the web path triggers SW registration of the hashed
`/assets/sw-download-*.js`, which throws
`SecurityError: ... scope ('/') is not under the max scope allowed ('/assets/')`. The browser
download event never fires; app logs `download failed: SecurityError`.

**Cause:** the SW needs the `Service-Worker-Allowed: /` response header to claim a scope above
its own path. `stream-download.ts` notes only the Caddy config ships that header; the
`vite preview` server (and the Electron `anyfs://` protocol) do not. So under Playwright's
preview-served prod build, the streaming SW download cannot register.

**Repro:** web app (vite preview) → mount any FS → open a file to download → SecurityError.

**Suspected area:** partly environment (preview server header), partly app (download path
assumes a `/`-scoped SW that only Caddy enables). For E2E: either (a) serve the preview with
`Service-Worker-Allowed: /` (a preview-middleware/header tweak), or (b) treat web download as a
known-fixme and assert the Electron IPC download instead. Decision pending.

---

## F5 — native whole-disk btrfs (no partition table) not auto-detected (CONFIRMED native, Medium)

**Observed (Phase 3.2 spike, native LKL):** Opening `btrfs-whole.vmdk` (whole-disk btrfs, no
PT) natively, `sessionListJson` returns 0 partitions and `sessionEnter(0)` fails cleanly
(`Error: operation: rc=-1`) — it does **not** hang. The whole-disk fstype-hint probe in
`src/core/anyfs_session.c` (`anyfs_session_open`, ~lines 272-292) only recognizes
ext4/xfs/ntfs/exfat superblock magics, so a whole-disk btrfs falls into NULL-hint
auto-detection (`anyfs_session_enter` part==0 path) which never attempts btrfs. **btrfs inside a
partition mounts fine natively** (verified with a GPT+btrfs-partition image →
`BTRFS info (device vda1): first mount`, listed contents). So this is a missing btrfs magic in
the open-time hint probe, not a btrfs driver problem.

**Repro (native):** open a whole-disk (no-PT) btrfs image → enter(0) → rc=-1 instead of mount.

**Suspected fix area:** add the btrfs superblock magic (`_BHRfS_M` at offset 0x10040) to the
whole-disk hint detection in `anyfs_session.c`. Cleanly errors today, so low urgency.

**E2E impact:** the `btrfsVmdk` fixture's whole-disk index-0 mount will be a `test.fixme` on
BOTH backends (wasm hangs F3 / native errors F5). Tests should use btrfs-in-a-partition if a
passing native btrfs case is wanted later.

---

## F6 — IndexedDB never settles on the `anyfs://` origin → "Open file…" hang (✅ FIXED — time-boxed openDb, was High)

**Observed (Phase 3.3, Electron prod renderer over `anyfs://app`):** `indexedDB.open(...)` on the
`anyfs://app` origin fires **neither** `onsuccess`, `onerror`, **nor** `onblocked` — the request
hangs forever (`typeof indexedDB === 'object'`, so the API is present; the open just never
settles). Because `recents.ts`'s `openDb()` awaits that request, every recents write hangs too.
The killer is `FilePicker.onOpenFile`: in native mode it does
`const p = await electronDialog.openImage(); … await addRecentPath(p, name); onSource(...)`. The
`dialog:openImage` IPC returns the path correctly (verified: returns the real
`ANYFS_TEST_LOCAL_PATH`), but `addRecentPath` → `openDb()` hangs, so **`onSource` is never
reached** and the disk never loads. The picker just sits there with no error. `onSubmitUrl`
(addRecentUrl) and drops (addRecentFile) hit the same wall.

`listRecents()` also calls `openDb()` but wraps it in try/catch with no timeout, so on mount it
hangs silently in the background (fire-and-forget useEffect) and the picker still renders — only
the _await-in-critical-path_ writes are fatal.

**Repro (Electron):** launch the packaged/prod renderer (`anyfs://app`, not the Vite dev server)
→ click "Open file…" (native dialog auto-answered via `ANYFS_TEST_LOCAL_PATH`) → the path comes
back but the picker never transitions to the disk view. Driver-side repro:
`win.evaluate(() => new Promise(r => { const q = indexedDB.open('x',1); q.onsuccess=()=>r('ok'); q.onerror=()=>r('err'); setTimeout(()=>r('TIMEOUT'),8000); }))`
→ resolves `TIMEOUT` on `anyfs://app`.

**Suspected area:** IndexedDB backing store for the privileged `anyfs://` custom scheme in
Electron 42's Chromium. The main process even logs
`service_worker_storage.cc … Failed to delete the database: Database IO error` at startup, which
points at storage-partition trouble for this origin. Likely the custom scheme isn't getting a
usable quota/storage partition, or the registration is missing a privilege. Either register the
scheme with whatever storage privilege IDB needs, serve the renderer from an `http(s)`-backed
origin, or make `recents.ts` time-box/guard `openDb()` so a dead IDB degrades to "no recents"
(as `listRecents` already does) instead of wedging the open flow.

**E2E impact / driver workaround:** `ElectronDriver.openImage` does **not** click the open
button. It calls the `__anyfsTest.openPath(path)` bridge (App.tsx), which sets the **same**
`{kind:'path', path}` source the button would (`path === ANYFS_TEST_LOCAL_PATH`) but skips the
broken recents write. This still drives the **real** native attachPath/listParts pipeline — only
the IndexedDB recents bookkeeping is bypassed. The native disk attach + partition listing was
verified real this way (multi.img → partitions `[0,1,2]`, `backendMode==='native'`).

---

## F7 — native `sessionEnter` (partition mount) hangs/crashes in the Electron main process (✅ FIXED — sync enter, was High)

**Observed (Phase 3.3, Electron native backend):** With the native addon active in Electron,
`sessionOpen` + `sessionListJson` work (multi.img → `vda: vda1 vda2`, partitions `[0,1,2]`
listed, `mode==='native'`), but selecting **any** partition wedges. DiskView shows
"mounting partition #N…" indefinitely; `session.enter(N)` (IPC `anyfs-native:diskEnter` →
addon `sessionEnter` AsyncWorker) **never resolves or rejects**. `getState().mountPath` stays
null forever and no mount-error surfaces. The LKL kernel log shows **no** `EXT4-fs (vda1):
mounted` line at all after the enter — the mount stalls before completion. Reproduced for
partition #1 (ext4) and partition #2 (vfat) → it's not fs-specific; it's the enter/mount op
itself.

**Critical isolation:** the **same** addon `.node` source, driven directly under **plain Node**
(`kernelInit; sessionOpen; await sessionListJson; await sessionEnter(h,1,1); await readdirJson`),
mounts ext4 **cleanly**: `EXT4-fs (vda1): mounted filesystem … ro without journal`,
mountPath `/lklmnt/anyfs_d0_p1`, readdir lists `empty/dir/lost+found/hello.txt/link`. So the
AsyncWorker enter path is correct in isolation — the deadlock is **specific to the Electron main
process**, where libuv's thread pool is integrated with Chromium's message loop. The mount op
runs on a libuv worker thread and the LKL mount needs to spawn an in-kernel kthread (a real
host thread); that spawn never makes progress under Electron's loop integration, so the
AsyncWorker hangs holding the addon mutex. This is the native analog of the (now-fixed) wasm F1
kthread-spawn-while-blocked deadlock, and it landed with the recent AsyncWorker conversion
(`3388c5b` "convert IO-heavy ops to AsyncWorker"): `sessionEnter` moved off the main thread.

**Repro (Electron):** native backend → open multi.img → click partition #1 or #2 → hangs on
"mounting…", forever, no error. **Counter-repro (Node, passes):** see
`packages/anyfs-native/test/` pattern — init/open/listJson/**enter**/readdir all resolve.

**Suspected fix area:** `packages/anyfs-native/src/binding.cc` `sessionEnter` AsyncWorker vs.
Electron's libuv/Chromium loop. Options mirror the wasm fix: run the enter/mount on a dedicated
host thread that keeps an event loop free to service the kthread spawn, or revert `sessionEnter`
to a synchronous op on the QEMU-affine thread (it was sync pre-`3388c5b`). Whole-disk enter(0)
likely hits the same wall.

**ROOT CAUSE (gdb, 2026-05-30) — it is NOT the threading/AsyncWorker, NOT QEMU BQL.** The
SIGABRT backtrace is conclusive:
```
electron: ../util/fdmon-io_uring.c:99: get_sqe: Assertion `ret > 1' failed.
#4 get_sqe → #5 add_poll_add_sqe → #9 aio_dispatch → #10 aio_ctx_dispatch
→ #13 g_main_context_iteration   (Electron's GLib main loop)
```
QEMU's AioContext registers itself as a **GLib source**. QEMU built with
`CONFIG_LINUX_IO_URING` uses the **io_uring fdmon** backend (`aio_context_setup` prefers it when
available). In the Electron **main process**, Electron's own **GLib main loop**
(`g_main_context_iteration`) dispatches QEMU's AioContext source — while QEMU *also* drives the
same io_uring ring from its own `aio_poll` (`bdrv_poll_co`). Two drivers on one io_uring SQ ring
→ submission-queue exhaustion → `assert(ret > 1)` → abort. Plain Node / `ELECTRON_RUN_AS_NODE`
do NOT crash because they use a libuv loop that does not adopt QEMU's GLib source — only ONE
thing drives the ring. Verified: under `ELECTRON_RUN_AS_NODE` the **same addon mounts ext4
cleanly** (`/lklmnt/anyfs_d0_p1`, exit 0). So the fault is the QEMU-libblock-AioContext ↔
Electron-GLib-main-loop entanglement, independent of how `sessionEnter` is threaded (the
`3388c5b` AsyncWorker change only changed the crash's *timing*, not its existence).

**Attempted fix 1 (dedicated pthread, mirroring wasm):** still crashes — the AioContext is still
dispatched by Electron's GLib loop regardless of which thread submits the syscall.
**Attempted fix 2 (force epoll fdmon via `ANYFS_NO_IO_URING` env + QEMU `aio_context_setup`
patch):** the io_uring assert is GONE (confirms the root cause), but the epoll path then
**SIGSEGVs** during enter under Electron (timing-sensitive: segv without gdb, hang under gdb) —
a second, race-shaped manifestation of the same fundamental problem.

**ARCHITECTURAL CONCLUSION:** Embedding QEMU's block layer (which owns an AioContext/event loop)
*inside the Electron main process* fundamentally conflicts with Electron's GLib/Chromium main
loop — every fdmon backend hits a different fatal interaction. The correct fix is **process
isolation**: run the native addon (LKL kernel + QEMU libblock) in a **separate process** with its
own libuv loop (Electron `utilityProcess`, or a child Node process), and IPC results to the main
process — exactly the context (`ELECTRON_RUN_AS_NODE`) already proven to work.

**✅ FIX APPLIED (sync enter):** `sessionEnter` reverted to a SYNCHRONOUS N-API call (like
`sessionOpen`/`kernelInit`) under `g_op_mutex`, instead of the `3388c5b` libuv-pool AsyncWorker.
Running the mount inline on the calling (main) thread keeps QEMU's AioContext work *within* the
blocking call rather than being dispatched later by Electron's GLib loop — so the io_uring
`get_sqe` / epoll fdmon collision never occurs. Verified end-to-end through the real Electron
renderer + ElectronDriver: ext4 partition **mounts**, browser **lists**
`dir/empty/hello.txt/link/lost+found`, and `hello.txt` **downloads** via Electron IPC (13 bytes,
`mechanism: electron-ipc`); also re-verified ext4 still mounts under plain Node (no regression).
Tradeoff accepted: the mount briefly blocks the JS main thread during the mount (same as
pre-`3388c5b` and as `sessionOpen` already does) in exchange for correctness. The
async/utilityProcess isolation route above remains the longer-term option if main-thread
blocking during mount becomes a UX problem. (The wasm backend was separately fixed for F1/F3.)

**E2E impact:** the **native** Electron backend can attach a disk and list partitions but cannot
mount any partition today, so native `enterPartition`/`listRows`/`download`-of-real-bytes are
`test.fixme` until this is fixed. The driver itself is verified correct: native attach + partition
list are real; and the Electron **IPC download mechanism** (`electronDownload.open/write/close` →
`download:open` writing to `ANYFS_TEST_DOWNLOAD_DIR`) was verified end-to-end independently
(13 bytes "hello, world\n" written + read back, `mechanism: 'electron-ipc'`). The full
mount→activate→download chain just can't complete in-app until F7 (and, for the picker button,
F6) are resolved. Note: the **wasm** Electron backend was also blocked for file sources by a
separate quirk (now **F8, fixed** — see below).

---

## F8 — Electron **wasm** backend rejected every local file (✅ FIXED, was High — blocked all electron-wasm flow specs)

**Observed (Phase 3.4, electron-wasm wire smoke):** With `ANYFS_DISABLE_NATIVE=1` (the
`electron-wasm` project), `window.anyfsNative` is correctly absent and `getState().mode==='wasm'`,
so `ElectronDriver.openImage` correctly takes its wasm branch and feeds the image through the
hidden legacy `<input type=file>` (streamed from disk, like WebDriver). But the renderer then
errored `source kind "path" is not supported by the wasm backend` and `status` went to `error`.

**Root cause (two compounding bugs):**

1. `examples/vite-demo/src/utils.ts` `fileToSource()` converted EVERY File to `{kind:'path'}`
   whenever `window.electronFile.pathFor` existed — but the `electronFile` bridge is exposed by
   preload in BOTH native and wasm modes. So even in wasm mode (native disabled), a picked/dropped
   File became a host-path source, which the wasm backend cannot consume.
2. `packages/core/src/dispatch.ts` `createSession('electron', …)` wasm fallback set
   `allowedKinds = {'url'}` only (comment: "blob is not [legal] (frontend resolves File→path
   before producing source)"). So even a `{kind:'blob'}` source would have been rejected — the
   electron-wasm path had **no** working local-file mechanism at all (and the vite-demo never wires
   the `pathLoopbackUrl` cap that would make `path` legal via the loopback proxy).

**Repro:** electron-wasm (`ANYFS_DISABLE_NATIVE=1`) → drop/pick any local image → "source kind
'path' is not supported by the wasm backend".

**✅ FIX APPLIED:**

- `utils.fileToSource` now gates the path conversion on `getAnyfsNative()` (the same
  `window.anyfsNative` signal `App.tsx` uses to choose `env`), not on `electronFile.pathFor`
  alone. Only the **native** backend (which can mount a host path directly) gets `{kind:'path'}`;
  in a plain browser OR in Electron-wasm the File stays `{kind:'blob'}` for the WORKERFS path.
- `dispatch.ts` electron-wasm `allowedKinds` now includes `'blob'` (`{'blob','url'}`, plus
  `'path'` only with the loopback-proxy cap). `WasmSession.attachBlob` mounts a File directly —
  exactly the web backend's path, which mounts ext4 since F1 was fixed.

Rebuilt `@anyfs/core` dist (`pnpm --filter @anyfs/core build`) since vite-demo consumes
`@anyfs/core/dist`. Verified: the electron-wasm wire smoke now opens multi.img and lists
partitions `[0,1,2]`; web and electron-native unaffected (web has no `anyfsNative` → still `blob`;
native doesn't use `fileToSource`). No regression in either.

---

## F9 — Electron **native** `app.close()` hangs ~2 min after a native mount, blowing fixture teardown (CONFIRMED native, Medium)

**Observed (Phase 4, electron-native, open-browse-download flow):** Every test BODY on the
native backend PASSES in isolation — `flows/open-browse-download.spec.ts` run one test at a time
under `--project=electron-native` is green for all three:

- `@smoke open multiRaw, enter ext4, see known files` → passes (mount → partitions `[0,1,2]` →
  enter #1 → lists `dir/empty/lost+found/hello.txt/link`);
- `properties of hello.txt show a file with a size` → passes (5.3s; right-click → Properties →
  modal renders kind+size);
- `download hello.txt yields 13 bytes via the right mechanism` → passes (35.3s; real
  `download:open/write/close` IPC writes 13 bytes, `mechanism === 'electron-ipc'`).

But running the **whole spec file** (3 tests, 1 worker, sequential) fails: the FIRST test to run
in a worker passes its body, then the `driver` fixture's teardown — `await app.close()` after a
native QEMU+LKL mount — **never returns within the 120 s per-test timeout**:

```
Tearing down "driver" exceeded the test timeout of 120000ms.
Worker teardown timeout of 120000ms exceeded.
Failed worker ran 1 test: … @smoke open multiRaw …
```

Playwright then recycles the worker and the remaining tests pass on the fresh process, so the
failing test is non-deterministic in WHICH test it lands on (it's whichever ran first), but the
teardown hang itself is deterministic. No Electron process is left leaked (it does eventually
die) — `app.close()` just doesn't complete in time, and the error is purely in fixture teardown,
never in a test assertion.

**Repro (electron-native):** `npx playwright test --project=electron-native
flows/open-browse-download.spec.ts` → one test reports `Tearing down "driver" exceeded the test
timeout`, worker recycled. **Counter-repro:** the same tests run individually
(`-g "<single title>"`) ALL pass; electron-wasm runs all 3 in one worker green (no native QEMU,
clean shutdown); web runs green.

**Suspected area:** the same QEMU-block-AioContext ↔ Electron-GLib-main-loop entanglement
diagnosed for F7, surfacing now at process **SHUTDOWN** rather than at mount. F7's sync-enter fix
kept the AioContext work inside the blocking mount call so the io_uring/epoll fdmon collision
never fires *during* enter — but tearing the app down while QEMU's block layer is still
registered as a GLib source apparently stalls the exit path. The architectural fix is the same
as F7's long-term recommendation: run the native addon (LKL + QEMU libblock) in a separate
process (`utilityProcess` / child Node) with its own libuv loop, so closing the Electron window
doesn't have to unwind a QEMU AioContext from Chromium's main loop.

**E2E impact / handling:** the flow spec marks the **electron-native** project `test.fixme` for
this whole spec (a `beforeEach` gate keyed on `testInfo.project.name === 'electron-native'`),
referencing this finding. Because the fixme aborts before the `driver` fixture is requested, no
Electron app is launched on native and the teardown hang can't fire — the suite stays green and
deterministic. **No assertion is weakened:** the native test bodies are verified to pass in
isolation, and the real 13-byte IPC-download proof (`mechanism === 'electron-ipc'`) still runs
green on the **electron-wasm** project (3/3), which exercises the identical
`download:open/write/close` IPC path. Web runs `@smoke` + `properties` (download skipped for F4).

---

## F10 — qcow2 not decoded on the wasm attach/attachUrl path (raw bytes -> no PT); QEMU IS in the bundle but not selected for WORKERFS/URLFS opens (symptom CONFIRMED, root cause OPEN, Medium)

**Observed (Phase 5, web + electron-wasm, url-load flow):** Opening the trusty `qcow2` fixture
(either by URL through the local Range server *or* as a local `<input>` blob) attaches the source
and the disk reaches `status: 'ready'`, but `listParts` returns **only the whole-disk synthetic
index `[0]`** — never the real ext4 partition (index 1). Entering index 0 then probes the disk
as a *bare* filesystem and every probe fails in dmesg (`EXT4-fs (vda): VFS: Can't find ext4
filesystem`, `FAT-fs … bogus number of reserved sectors`, `exFAT-fs … invalid boot record
signature`, …), so the mount never produces a file list and `enterPartition` times out waiting
for either a `partition-1` button or a `file-entry` row.

**Smoking gun (dmesg):** the decoded block device is sized to the qcow2's **on-disk/compressed
file length, not its virtual size**:

```
[diag] attachUrl URLFS mounted, calling disk_open...
virtio_blk virtio0: [vda] 517377 512-byte logical blocks (265 MB/253 MiB)
```

`qemu-img info` reports the qcow2's *virtual* size as **2.2 GiB (2361393152 bytes)** with a real
MBR (one type-0x83 Linux partition at LBA 2048 → byte 1048576, host-verified by loop-mounting:
root has `bin boot dev etc home lib … etc/hostname`). But `vda` is **253 MiB** — exactly the raw
file length (264897024 bytes). So the qcow2 is handed to the kernel as a RAW image of its
*compressed* bytes; the qcow2 L1/L2 cluster mapping is never applied. The sector-0 bytes are the
qcow2 header, not an MBR, hence no partition table and no mountable filesystem.

**Root cause — CORRECTED (initial diagnosis was wrong):** The bundle is NOT missing QEMU.
Verified: `ts/packages/core/wasm/anyfs.wasm` **contains qcow2/bdrv decode symbols**
(`qcow2_cache_entry_mark_dirty`, `qcow2_mark_dirty`, `bdrv_*`), and `build_anyfs_wasm.sh` always
links the QEMU objects (`ANYFS_QEMU_OBJ`/`QEMU_BLK_OBJ` in `EXTRA_OBJS`, not env-gated). The
single `anyfs.mjs` IS the unified QEMU-enabled bundle — commit `d7bbf51`
*"drop anyfs.qemu.* filename — single unified bundle"* deliberately removed the separate
`anyfs.qemu.mjs`. (The memory note `feedback_anyfs_qemu_bundle_rebuild` predates that unification
and is now stale on the filename.)

So the real cause is NOT a missing bundle. The qcow2 decode capability is present but is **not
being applied on the wasm `attach`/`attachUrl` path**: the worker mounts the image via
WORKERFS/URLFS (presenting it as a *file*) and calls `anyfs_ts_session_open_p(fsPath, 1)`, and on
this path the image is opened as a **raw** block device rather than routed through the QEMU
backend (`qemu_blk_open`/`blk_new_open`) the way the *native* addon does (native logs show
`[qemu_blk] blk_new_open` decoding correctly). The exact gap — whether the wasm
`anyfs_ts_session_open` should detect the qcow2 magic and select `qemu_backend_ops`, or whether
the worker must pass a backend/format hint — is **not yet root-caused** (needs a dedicated dive
into `anyfs_kernel.c` backend registration + `anyfs_session_open` backend selection under
`ANYFS_HAS_QEMU`). Independent of URLFS — identical `[0]`-only result opening the same qcow2 as a
local blob.

**Status:** symptom CONFIRMED, precise root cause OPEN. Not blocking the URLFS *mechanism* (which
works — source attaches, mounts, reaches ready). A raw-image URL fixture would give url-load a
green gate independent of this (the partition scanner handles raw images, cf. multiRaw `[0,1,2]`).

**Repro (web):** `npx playwright test --project=web --grep-invert @network
flows/url-load.spec.ts` (remove the `test.fixme` gate) → `enterPartition(1)` times out;
`listPartitionIndices()` returns `[0]`; dmesg shows `[vda] … 253 MiB` and every fs probe failing
on `vda`. **Counter-repro:** the raw `multiRaw` (multi.img) image lists `[0,1,2]` and mounts ext4
fine — only the *qcow2-decoded* path is broken.

**E2E impact / handling:** `flows/url-load.spec.ts` marks the whole spec `test.fixme(true, …)`
referencing this finding (the qcow2-decode gap blocks the only fixture wired for the URL flow).
**No assertion is weakened.** Two genuine harness/accuracy fixes made while triaging F10 stand on
their own and are kept:

- **CORS on the local Range server** (`fixtures/range-server.ts`): URLFS's in-worker `fetch`/XHR
  is cross-origin (page `http://localhost:4199` / `anyfs://…` vs server `http://127.0.0.1:<port>`),
  so without `Access-Control-Allow-Origin` the browser blocked every byte
  (`blocked by CORS policy`) before URLFS could read. Added `Access-Control-Allow-Origin: *`,
  `OPTIONS` preflight, and `Access-Control-Expose-Headers: Content-Range, Accept-Ranges,
  Content-Length` to both `serveFileWithRange` and `serveFileNoRange`. With this, the URL open
  provably works end-to-end: source attaches, URLFS mounts, `vda` appears, disk → `ready`.
- **Manifest accuracy** (`fixtures/manifest.ts` `qcow2Url`): host loop-mount confirmed the ext4
  partition is index 1 and `etc/hostname` (7 bytes, "ubuntu") exists, so the manifest's
  `{index:1, fs:'ext4', tree:[{path:'etc/hostname'}]}` is correct and unchanged.

**Suggested fix:** build the `ANYFS_QEMU=1` wasm bundle and point `App.tsx` `wasmModuleName` at
`anyfs.qemu.mjs` (or select it by image format). Once qcow2 decodes through the qemu block layer,
`vda` becomes the 2.2 GiB virtual disk, the MBR is parsed, `listParts` reports index 1, and the
ext4 partition mounts — at which point this spec's `test.fixme(true, …)` can be lifted.

---

## Phase 7 — error / edge-case flow: NO new defects (graceful failure CONFIRMED on web + electron-wasm)

`flows/errors.spec.ts` exercises two failure paths and asserts the app surfaces an error rather
than hanging or crashing. Both fail **gracefully** on web and electron-wasm — no `test.fixme`
weakening, no F11. Recorded here for the record (and so a future regression to a *hang* is
recognised as the loss it would be):

- **Corrupt image (1 MiB of zeros, `fixtures/bad-image.ts`):** the partition scanner offers only
  the synthetic whole-disk index `[0]` (no PT, as expected). Entering it probes the bare bytes,
  finds no filesystem, and surfaces the inline DiskView **"Can't mount partition #N"** error —
  `enterPartition(0)` never resolves into a file list (no bogus rows). Notably the global
  `getState().status` stays `'ready'` (the whole-disk *attach* succeeded; the per-partition mount
  failure renders inline in DiskView, it does NOT flip the session status to `'error'`), so the
  surface that fires is the "Can't mount partition #" text, not `status==='error'` and not the
  url-error-dialog. `driver.expectError('mount-failed')` catches it via that text arm. This is the
  clean behaviour F2 introduced (the wasm no-FS hang is gone). No hang on web or electron-wasm.

- **URL without Range support (`serveFileNoRange`):** the in-worker URLFS does a HEAD (no
  `Accept-Ranges`), then a probe `Range: bytes=0-0` GET; the server answers **200** (ignoring
  Range) instead of 206, so `url-fs.ts probeUrl` throws
  `URLFS: server lacks Accept-Ranges: bytes (got status 200 for probe range)` during mount. The
  worker rejects, the renderer goes to `getState().status === 'error'` (error.message carries the
  URLFS throw), and `driver.expectError('no-range')` catches it via the `status==='error'` arm.
  No url-error-dialog on the programmatic `openUrl` path (that dialog is the FilePicker's
  submit-time validator), and — importantly — **no hang**: URLFS detects no-range up front rather
  than wedging on a non-seekable stream. Confirmed on web AND electron-wasm (electron-wasm runs the
  same in-worker URLFS, native disabled, so no main-process proxy masks it).

**Backend matrix:** web 2/2 pass · electron-wasm 2/2 pass · electron-native 2/2 skipped (F9-gated
in a `beforeEach`, consistent with the other flow specs; these cases don't mount successfully so
F9 likely wouldn't fire, but native is gated off to never risk the 2-min `app.close()` hang — the
error behaviour is fully covered by web + electron-wasm).
