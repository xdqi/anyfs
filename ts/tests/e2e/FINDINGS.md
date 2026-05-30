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

## F6 — IndexedDB never settles on the `anyfs://` origin → "Open file…" / "Open URL…" hang in packaged Electron (CONFIRMED, High)

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

## F7 — native `sessionEnter` (partition mount) hangs in the Electron main process — but NOT under plain Node (CONFIRMED, High)

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

**E2E impact:** the **native** Electron backend can attach a disk and list partitions but cannot
mount any partition today, so native `enterPartition`/`listRows`/`download`-of-real-bytes are
`test.fixme` until this is fixed. The driver itself is verified correct: native attach + partition
list are real; and the Electron **IPC download mechanism** (`electronDownload.open/write/close` →
`download:open` writing to `ANYFS_TEST_DOWNLOAD_DIR`) was verified end-to-end independently
(13 bytes "hello, world\n" written + read back, `mechanism: 'electron-ipc'`). The full
mount→activate→download chain just can't complete in-app until F7 (and, for the picker button,
F6) are resolved. Note: the **wasm** Electron backend is also blocked for file sources by a
separate quirk — `utils.fileToSource` always calls `window.electronFile.pathFor(file)` (exposed
in both modes), converting every dropped/picked File into a `{kind:'path'}` source, which the
wasm backend rejects ("source kind 'path' is not supported by the wasm backend"). So in Electron,
wasm can only take URL sources, not files.
