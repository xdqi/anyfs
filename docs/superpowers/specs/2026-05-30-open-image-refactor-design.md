# Open-Image Flow Refactor — Design Spec

> Written 2026-05-30 from the brainstorming session that followed the flow audit in
> [`docs/open-image-flow.md`](../../open-image-flow.md). That audit is the canonical
> description of the **as-is**; this spec describes the **to-be**.

**Goal:** make the three session backends (Native, Wasm, Node) have clear capability
boundaries, a single dispatch entry point, unified UX, and non-blocking native IO.

---

## 0. Terminology — blob vs path

- `SessionSource` kind renamed: `{kind:'file', file}` → `{kind:'blob', blob: Blob}`.
  `{kind:'path'}` and `{kind:'url'}` unchanged.
- `AnyfsSession.attachFile` → `attachBlob` (interface, base class, all three
  implementations, worker ops, provider dispatch — every call site).
- User-facing UI copy keeps "file" / "Open file…" unchanged. Only the TS
  layer distinguishes `blob` (in-memory) from `path` (host absolute path).

---

## 1. Capability matrix

| backend | blob | path | url |
|---|---|---|---|
| **WasmSession** (web caps) | ✅ WORKERFS | ❌ | ✅ same-origin / CORS-granted only |
| **WasmSession** (electron caps) | ❌ | ✅ path → local loopback HTTP → URLFS | ✅ arbitrary (via `anyfs-url://` proxy) |
| **NativeSession** (electron only) | ❌ | ✅ direct `sessionOpen` | ✅ per-disk loopback proxy thread |
| **NodeWasmSession** (test/CI) | ❌ | ✅ NODEFS | ❌ |

Key: in Electron, regardless of backend, the frontend always resolves a picked /
dropped file to an **absolute path** (see §4). Each backend then turns that path
into bytes its own way — native calls `sessionOpen(path)` directly; wasm stands
up a local loopback HTTP server and feeds the URL to URLFS.

---

## 2. Dispatch — `createSession(env, caps)`

A pure factory in `@anyfs/core` returns `{ session, allowedKinds }` (or throws).
Decision table:

| env | caps | session class | allowed kinds |
|---|---|---|---|
| `web` | — | WasmSession (web caps) | blob, url (same-origin) |
| `electron` | native bridge available, disableNative off | NativeSession | path, url |
| `electron` | native bridge available, disableNative on | WasmSession (electron caps) | path, url |
| `electron` | native bridge unavailable | WasmSession (electron caps) | path, url |
| `node` | — | NodeWasmSession | path |

- `AnyfsProvider.mode` logic is replaced by calling this factory — no more
  inlined `getAnyfsNative()` + scattered kind-validity checks.
- `allowedKinds` is a `Set<SessionSource['kind']>`; the provider's source effect
  validates with `allowedKinds.has(source.kind)` before attach, throwing a clear
  error on mismatch.
- The factory is a ~30-line pure function, unit-testable without React / DOM /
  addon.

---

## 3. WasmSession caps

Single WasmSession class, no split. Constructor receives `WasmCaps`:

```ts
interface WasmCaps {
  urlProxyPrefix?: string;       // set → URLFS rewrites cross-origin URLs via anyfs-url://
  pathViaLoopback?: { port: number; token: string };  // set → attachPath maps path to local HTTP
}
```

- **web caps**: both fields absent → blob + same-origin/CORS URL only.
- **electron caps**: both fields present → path (via loopback HTTP + URLFS) + arbitrary URL (via `anyfs-url://`).

Behavior:
- `attachBlob` / `attachUrl` / `attachPath` all exist on the class; illegal
  (caps, kind) combos throw a clear error.
- Under electron caps, `attachPath` converts `path` → `http://127.0.0.1:<port>/<token>`
  and delegates to the URLFS path (same as `attachUrl` internally).
  **Loopback server lifecycle:** unified with the existing per-disk HTTP proxy
  mechanism. The existing IPC `anyfs-native:registerUrl` is renamed to
  `anyfs-native:startProxy` and extended to accept either `{upstreamUrl}` or
  `{localPath}` in its payload. The proxy worker thread ([http-proxy-worker.ts](../ts/examples/electron-demo/src/http-proxy-worker.ts))
  gains a local-file Range-serving path. `NativeSession.attachUrl` and
  `WasmSession(electron).attachPath` both go through this single IPC → single
  proxy worker pool. The token is a random per-session string. The server is
  stopped on `session.close()` via `anyfs-native:stopProxy` (currently
  `unregisterUrl`).
- `caps.urlProxyPrefix` is forwarded into the worker's boot message → worker-side
  `setUrlProxyPrefix` → URLFS's `applyUrlProxy` rewrites automatically. Under
  web caps no prefix is sent, so URLs pass through verbatim.

---

## 4. Electron drag-drop → native path + whole-window drop zone

### 4.1 Path resolution

- Preload exposes `electronFile.pathFor(file: File): Promise<string>` via
  `contextBridge`, calling `webUtils.getPathForFile(file)`.
- FilePicker frontend code is the **same** in web and Electron: it always gets a
  `File` object first (FSA picker / drag-drop / legacy `<input>`).
- Before producing the source, one extra step: if the electron bridge is present,
  call `pathFor(file)` to translate the `File` into an absolute path string. The
  source then becomes `{kind:'path'}`. In plain web (no bridge), the source
  stays `{kind:'blob'}`.
- This step is ≤10 lines in FilePicker.

### 4.2 Drag-drop UX

- Listeners move from the FilePicker card to **the whole window** (document-level
  `dragenter`/`dragover`/`dragleave`/`drop`).
- While dragging: show a **full-window translucent overlay** (`pointer-events: none`)
  with a "Drop image here" prompt.
- On drop: if a source is already loaded → show `ConfirmDialog` "Replace current
  image?"; confirm → `setSource`, cancel → no-op. No loaded source → open
  immediately.

---

## 5. Always show the partition page (with #0 whole-disk entry)

- `AnyfsProvider.autoMount` is removed. The provider no longer auto-calls
  `enter(0)` — it only runs `listParts()` and leaves `mountPath` null.
- `DiskView` **always** renders the partition picker page. The list consists of:
  - A **#0 = Whole disk** entry (always present).
  - Every partition returned by `listParts()` (index ≥ 1).
- Click any entry → `session.enter(index)`. With no real partitions the user
  only sees #0, and clicking it is equivalent to the old autoMount `enter(0)` —
  but the path is now unified.

---

## 6. Mode switch → restart Electron process

- Trigger: user toggles the `disableNative` checkbox in Settings → a confirm
  dialog appears: "Changing this requires restarting the app. Restart now?".
  - **Confirm**: write the new `disableNative` value to localStorage (so the
    relaunched process reads it), then invoke `settings:relaunch` IPC.
  - **Cancel**: do not call `Settings.update` — the checkbox visually reverts
    because React state was never committed. No localStorage write, no dialog
    re-appears. (Implementation: the Toggle's `onChange` fires a two-phase flow:
    preview the UI toggle → show dialog → on confirm, call `update` + IPC; on
    cancel, reset the local preview state.)
- Mechanism:
  - Renderer invokes a new IPC channel `settings:relaunch`.
  - Main-process handler calls `app.relaunch()` (preserves argv) then
    `app.exit()`.
  - The new process starts with the updated `disableNative` in localStorage;
    the provider reads the correct `forceMode` at mount.
- Side effect: the native LKL kernel is destroyed with the old process — no
  reference-counting or halt logic is needed in the addon (kernel lifecycle =
  process lifecycle; the idempotent `init` guard is sufficient).
- The `disableNative` checkbox is **not rendered** in web (`useSettings`'s
  `nativeAvailable` already depends on `getAnyfsNative()`, which is always falsy
  in a browser).

---

## 7. Non-blocking native addon (Napi::AsyncWorker)

### 7.1 Problem

The binding.cc exports are entirely synchronous — e.g. `KernelInit` calls
`anyfs_ts_kernel_init` and returns the result directly. Every addon call blocks
the calling JS thread (Electron main process) for the duration of the C operation.
A large `pread` or `readdirJson` freezes the main-process event loop.

### 7.2 Design

1. **Wrap each addon export in a `Napi::AsyncWorker` subclass.**
   - `Execute()` runs in a libuv thread-pool thread → calls the `anyfs_ts_*` C
     entry.
   - `OnOK()` runs back on the JS thread → invokes the callback, resolves the Promise.
   - Affected exports: `kernelInit`, `sessionOpen`, `sessionClose`, `sessionListJson`,
     `sessionMetaJson`, `sessionEnter`, `readdirJson`, `lstatJson`, `statJson`,
     `readlink`, `realpath`, `readKernelFile`, `fileOpen`, `pread`, `fileClose`,
     `kernelHalt`.

2. **C++-layer serialization.**
   - LKL holds a single CPU lock; two concurrent ops would crash the kernel.
   - A `std::mutex` + queue ensures only one op runs at a time. While one op is
     executing in the thread pool, the next JS call queues (its Promise stays
     pending until the previous op's `OnOK` releases the lock).
   - This is scheduling serialization, not a barrier — the JS main thread is
     never blocked.
   - Initial strategy: treat **all guarded ops as a single critical section**
     (simplest, guaranteed correctness). Future refinement: split into
     read-only / per-disk-handle groups.

3. **main.ts changes.**
   - `AnyfsNativeModule` type signatures become **fully async** (return
     `Promise<number>` instead of `number`).
   - Every IPC handler that calls the addon adds `await`.
   - IPC channels and the preload bridge surface **do not change** — `ipcMain.handle`
     already awaits the returned Promise automatically.

4. **Risk mitigation — experimental verification gate.**
   - LKL/QEMU internal thread-local assumptions are not fully characterized.
   - Before converting all ops: test one op with AsyncWorker, then test two
     concurrent ops to validate serialization. If the kernel panics, fall back
     to a single-threaded worker_thread approach for native (escalation path).
   - This verification is a **blocking prerequisite** in the implementation plan.

### 7.3 Non-goal

WasmSession and NodeWasmSession are not affected — WasmSession is already
non-blocking (worker thread + ASYNCIFY), and NodeWasmSession is test-only.

---

## Rejected alternatives

- **Splitting WasmSession into WebWasmSession / ElectronWasmSession**: would
  duplicate nearly identical attach logic. Caps on a single class is DRYer and
  the capability differences are small enough to express as flags.
- **Merging NodeWasmSession into WasmSession**: the transports are fundamentally
  different (direct synchronous ccall vs worker + postMessage + ASYNCIFY).
  Forcing them into one class would add internal branching and blur the dispatch
  — the opposite of the stated goal.
- **Strict per-session native kernel lifecycle**: the addon is a process-wide
  singleton LKL kernel with no multi-instance support. Per-session init/halt is
  physically impossible without moving the kernel into a child process. The
  `app.relaunch()` approach (§6) achieves the same user-visible effect (clean
  state after mode switch) without addon refactoring.
