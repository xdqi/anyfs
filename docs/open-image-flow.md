# Opening an Image: End-to-End Flow (Electron native + Web/wasm)

> This is a **flow / architecture reference**, not an implementation plan. Its goal is to give the
> single core path — "open a disk image" — one trustworthy mental model that matches the real code.
> Every hop cites `file:line`.
>
> Written 2026-05-30 from a code sweep of the then-current HEAD. Where a rename / branch contract is
> involved, the code is authoritative — if the code changes, trust the code and update this doc.

---

## 0. One-sentence mental model

The user picks a "source" (local file / URL / host path) → `AnyfsProvider` decides whether to use the
**native** or **wasm** backend → it attaches the source into a **single process-wide LKL kernel** →
lists partitions → enters a partition (or the whole disk) to get an LKL mount path → the UI does
readdir/read under that path.

**Both backends share one `AnyfsSession` interface** ([session.ts](../ts/packages/core/src/session.ts));
the difference is only the transport:
- **native** = a real LKL kernel in the Electron main process (N-API addon); the renderer drives it over an IPC bridge.
- **wasm** = an emscripten LKL kernel in a dedicated Web Worker inside the renderer; driven over postMessage.

---

## 1. Three key abstractions (get these straight first)

### 1.1 Source — what the user picked
`SessionSource` in [types.ts](../ts/packages/core/src/types.ts), three kinds:

| kind | shape | native can open? | wasm can open? |
|---|---|---|---|
| `{kind:'file', file}` | browser `File`/`Blob` | ❌ (addon can't take a Blob, only absolute paths) | ✅ WORKERFS mounts the Blob |
| `{kind:'url', url, name?}` | http(s) image | ✅ via a loopback HTTP proxy thread | ✅ via URLFS (sync XHR + Range) |
| `{kind:'path', path, name?}` | host absolute path | ✅ direct `sessionOpen(path)` | ❌ (sandbox can't see the host FS) |

**This table is a source of confusion:** the same kind takes completely different code on each backend,
and some (kind, backend) combinations throw outright.

### 1.2 Session — the unified interface
`AnyfsSession` in [session.ts](../ts/packages/core/src/session.ts),
abstract base [session-base.ts](../ts/packages/core/src/session-base.ts) `AnyfsSessionBase`
(provides `openReadable`/`walk`/fd tracking/`close` lifecycle), two implementations:
- [native-session.ts](../ts/packages/core/src/native-session.ts) `NativeSession`
- [wasm-session.ts](../ts/packages/core/src/wasm-session.ts) `WasmSession`

Pick one of three attach methods: `attachFile` / `attachUrl` / `attachPath`. Each implementation throws
a clear error on an unsupported kind:
- `WasmSession.attachPath` → `'WasmSession: attachPath not supported in browser wasm mode'` ([wasm-session.ts:149](../ts/packages/core/src/wasm-session.ts#L149))
- `NativeSession.attachFile` → `'NativeSession: attachFile(File) not supported ...'` ([native-session.ts:82](../ts/packages/core/src/native-session.ts#L82))

### 1.3 Backend mode — which path to take
`AnyfsProvider` decides this **once, at mount time**, and it **never changes** for the session
([provider.tsx:92-95](../ts/packages/react/src/provider.tsx#L92-L95)):

```
forceMode wins; otherwise getAnyfsNative() truthy → 'native', falsy → 'wasm'
```

⚠️ **This is the root cause of "disableNative requires a page reload":** `mode` is computed once via
`useState(initializer)`. Toggling the setting without reloading leaves the provider on the old mode
(the [Settings.tsx:162](../ts/examples/vite-demo/src/Settings.tsx#L162) UI copy already says to reload).

---

## 2. Overall sequence (the two paths side by side)

```
user action            FilePicker            AnyfsProvider           Session impl                    underlying kernel
─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
click Open file…/URL…  decide nativeMode     prewarm (boot kernel)   boot()                          init LKL
  → onSource(source)   produce SessionSource  source effect fires      attachFile/Url/Path             session_open
                                              listParts (autoMount)   listParts()                     session_list_json
click a partition      setSelectedPart       —                        enter(part)                     session_enter → mount path
                       DiskView renders tree  —                        readdir/openReadable            readdir_json / open+pread
```

- `nativeMode` is computed by FilePicker using the **live** `useSettings()`
  ([FilePicker.tsx:37](../ts/examples/vite-demo/src/components/FilePicker.tsx#L37)):
  `!!getAnyfsNative() && !settings.disableNative`.
- The provider's `mode` is computed from the `forceMode` **snapshotted at mount**.
- **The two can disagree** (FilePicker reads live, provider reads a snapshot) — yet another source of
  confusion; on the normal path they converge after a reload.

---

## 3. Entry layer: how FilePicker produces a Source

File: [FilePicker.tsx](../ts/examples/vite-demo/src/components/FilePicker.tsx)

| user action | nativeMode=true (Electron) | nativeMode=false (browser/wasm) |
|---|---|---|
| Open file… | `electronDialog.openImage()` → absolute path → `{kind:'path'}` ([L113-122](../ts/examples/vite-demo/src/components/FilePicker.tsx#L113-L122)) | FSA `pickFile()` or hidden `<input>` → `{kind:'file'}` ([L124-132](../ts/examples/vite-demo/src/components/FilePicker.tsx#L124-L132)) |
| Open URL… | no pre-probe, straight to `{kind:'url'}` (proxy thread spins up only at attach time) ([L143-150](../ts/examples/vite-demo/src/components/FilePicker.tsx#L143-L150)) | `probeUrlAhead` first (CORS/404 surfaces here) → `{kind:'url'}` ([L152-159](../ts/examples/vite-demo/src/components/FilePicker.tsx#L152-L159)) |
| drag-drop a file | rejected, prompts to use Open file… (addon needs an absolute path) ([L71-79](../ts/examples/vite-demo/src/components/FilePicker.tsx#L71-L79)) | FSA handle or `dataTransfer.files` → `{kind:'file'}` |
| Open system drive… | `electronDrives` lists disks → pick → `{kind:'path'}` (native only) ([L324-337](../ts/examples/vite-demo/src/components/FilePicker.tsx#L324-L337)) | this entry is not rendered |

Once produced, everything calls `onSource(source)` → App's `setSource` → passed as a prop to `AnyfsProvider`.

---

## 4. Provider layer: boot → attach → autoMount

File: [provider.tsx](../ts/packages/react/src/provider.tsx)

### 4.1 prewarm (boot the kernel, no attach)
`startPrewarm` ([L119-197](../ts/packages/react/src/provider.tsx#L119-L197)) branches by `mode`:
- **native**: `prewarmNative({memMb, loglevel})` ([index.ts:120](../ts/packages/core/src/index.ts#L120))
  → `getAnyfsNative()` gets the bridge → `bridge.available()` → `new NativeSession(bridge)` → `session.boot()`.
  When it returns `null`, the provider throws **`'native bridge unavailable'`** ([provider.tsx:140](../ts/packages/react/src/provider.tsx#L140)).
- **wasm**: `prewarm({workerUrl, ...})` ([index.ts:59](../ts/packages/core/src/index.ts#L59))
  → `new Worker(workerUrl, {type:'module'})` → wait for `host-ready` → `new WasmSession(worker)` → `callRaw('boot', ...)`.

State machine: `idle → booting → booted` ([AnyfsDiskStatus](../ts/packages/react/src/provider.tsx#L13-L20)).

### 4.2 source effect (attach + autoMount)
[L207-344](../ts/packages/react/src/provider.tsx#L207-L344), fires when `source` changes:
1. Reuse/start prewarm to get the session.
2. Dispatch attach by `source.kind`, throwing on illegal combinations ([L260-281](../ts/packages/react/src/provider.tsx#L260-L281)):
   - `file` and the session is a `NativeSession` → throws `'native backend cannot mount a File object ...'`
   - `path` and the session is not a `NativeSession` → throws `'host paths can only be opened in native mode ...'`
   - `url` → both backends go through `session.attachUrl`
3. When `autoMount` is true (on by default in the demo, [App.tsx:114](../ts/examples/vite-demo/src/App.tsx#L114)):
   `listParts()`; if there are **0 partitions** → `enter(0)` to mount the whole disk, and `mountPath` lands in state.
   With partitions present, nothing is mounted here — that's left to the user to pick in DiskView.

State machine continues: `attaching → mounting → ready` (on failure → `error`, exposed to the UI via `useAnyfsDisk().error`).

---

## 5. Backend A: NATIVE (LKL in the Electron main process)

### 5.1 Three layers + naming contracts (⚠️ historic bug hotspot)

```
renderer                        IPC channel                    main process                  addon export name
NativeSession (core)            'anyfs-native:<op>'           ipcMain.handle               anyfs_native.node
  ↓ this.bridge.init()    ──→   anyfs-native:init       ──→   m.kernelInit()         ──→   kernelInit (JS)
  ↓ bridge.diskOpen()     ──→   anyfs-native:diskOpen   ──→   m.sessionOpen()        ──→   sessionOpen
  ↓ bridge.diskListJson() ──→   anyfs-native:diskListJson──→  m.sessionListJson()    ──→   sessionListJson
  ↓ bridge.diskEnter()    ──→   anyfs-native:diskEnter  ──→   m.sessionEnter()       ──→   sessionEnter
```

**The three sets of names are three independent contracts — do not conflate them:**
1. **IPC channel strings** (`anyfs-native:diskOpen`, etc.) — the stable contract between preload and main;
   **do not touch when renaming the API.**
   Defined in [preload.ts:64-107](../ts/examples/electron-demo/src/preload.ts#L64-L107) and [main.ts installAnyfsNativeIpc](../ts/examples/electron-demo/src/main.ts#L408).
2. **bridge method names** (`bridge.init/diskOpen/...`) — core's `AnyfsNativeBridge` interface
   ([native-session.ts:5-24](../ts/packages/core/src/native-session.ts#L5-L24)) and the object preload exposes; must correspond one-to-one.
3. **addon export names** (`kernelInit/sessionOpen/...`) — the `m.<name>()` calls in main.ts; must match
   [binding.cc's `exports.Set(...)`](../ts/packages/anyfs-native/src/binding.cc#L264) exactly.

> **The 2026-05-30 bug was in contract #3:** the session-API refactor (commit ce451cb) renamed the addon
> exports `init→kernelInit`, `disk*→session*`, deleted `mountWhole`, and updated binding.cc and the addon
> test — **but missed main.ts**. main.ts still called `m.init()` → runtime `TypeError: m.init is not a function`
> → all native opens failed. TS does not catch this (the addon is `require()`d and described by a hand-written
> type). Fixed and verified. See memory `feedback_electron_main_addon_names`.

### 5.2 native attach details per source kind

- **path**: `attachPath` → `bridge.diskOpen(path, 1)` → `sessionOpen`
  ([native-session.ts:75-80](../ts/packages/core/src/native-session.ts#L75-L80)). The most direct.
- **url**: `attachUrl` ([native-session.ts:88-101](../ts/packages/core/src/native-session.ts#L88-L101)):
  1. `bridge.registerUrl(url)` → IPC `anyfs-native:registerUrl`
     → main spawns a **per-disk HTTP proxy Worker thread** ([main.ts:438-473](../ts/examples/electron-demo/src/main.ts#L438-L473),
     thread body [http-proxy-worker.ts](../ts/examples/electron-demo/src/http-proxy-worker.ts)),
     which forwards Range requests to the upstream URL on `127.0.0.1:<random port>` (Node fetch, follows redirects + TLS).
  2. Returns `proxyUrl = http://127.0.0.1:<port>/`.
  3. `bridge.diskOpen(proxyUrl, 1)` → QEMU's curl driver connects to this loopback proxy.
  > Why a separate thread: addon calls **synchronously block** the calling thread's event loop, so the HTTP
  > server must have its own libuv loop.
- **file**: `attachFile` throws immediately (native does not support Blobs).

### 5.3 native kernel lifecycle
**Process-wide singleton**: `init` is idempotent (`nativeInitDone` guard, [main.ts:420-427](../ts/examples/electron-demo/src/main.ts#L420-L427)),
and all sessions share one kernel. That's why `NativeSession._dispose` **deliberately does not call kernelHalt**
([native-session.ts:220](../ts/packages/core/src/native-session.ts#L220)) — it only closes the disk handle and unregisters the URL proxy.

---

## 6. Backend B: WASM (a Web Worker inside the renderer)

### 6.1 Why it must live in a Worker
[worker.ts:1-17](../ts/packages/core/src/worker.ts#L1-L17): (1) WORKERFS.mount() asserts
`ENVIRONMENT_IS_WORKER`; (2) LKL blocking syscalls → `Atomics.wait`, which Chrome forbids on the main thread.
(See memory `anyfs_browser_worker`.)

### 6.2 Message protocol
`postMessage({id, op, args})` → `postMessage({id, ok, result|error})`
([worker.ts:418-440](../ts/packages/core/src/worker.ts#L418-L440)).
Plus one-way events: `host-ready` / `progress` / `stdout` / `stderr` / `abort` / `host-error` / `host-rejection`
([wasm-session.ts:57-102](../ts/packages/core/src/wasm-session.ts#L57-L102)).
**All ops are serialized** (`opChain`, [worker.ts:416-440](../ts/packages/core/src/worker.ts#L416-L440)) —
LKL holds a single CPU lock and ASYNCIFY suspends the running export; two concurrent ops would trip a
`bad count while changing owner` panic.

### 6.3 op table → C entry points (the `_p` out-pointer variants)
Under ASYNCIFY the fiber switch discards the return value, so calls go through `_p` variants: the result is
written to a caller-supplied `int32_t*` out-pointer, and JS reads `HEAP32` after the await
([worker.ts:72-83](../ts/packages/core/src/worker.ts#L72-L83)).

| op (worker) | C entry (`anyfs_ts_*`) | notes |
|---|---|---|
| `boot` | `anyfs_ts_init_async` preferred, else `anyfs_ts_kernel_init` | dynamic-import the wasm shim → factory → init |
| `attach` (file) | `anyfs_ts_session_open_p` | WORKERFS mounts the Blob at `/work`, opened read-only |
| `attachUrl` | `anyfs_ts_session_open_p` | URLFS mounted at `/work` (see 6.4) |
| `listParts` / `meta` | `anyfs_ts_session_list_json_p` / `..._meta_json_p` | JSON out-buffer, doubles and retries if too small |
| `enter` | `anyfs_ts_session_enter_p` | writes the mount path into a buffer, returns `UTF8ToString` |
| `readdir`/`stat`/`statFollow` | `anyfs_ts_readdir_json_p` / `lstat_json_p` / `stat_json_p` | |
| `open`/`read`/`close` | `anyfs_ts_open_p` / `pread_p` / `close_p` | read uses a pre-allocated scratch buffer |
| `dispose` | `anyfs_ts_session_close` + `anyfs_ts_kernel_halt` | the wasm kernel is not shared, so dispose really halts |

> Unlike native: the wasm kernel is **per-worker**, so dispose calls `kernel_halt`.

### 6.4 the wasm URL path (completely different from native's proxy thread)
- The worker uses **URLFS** ([url-fs.ts](../ts/packages/core/src/url-fs.ts)): sync XHR + 512 KiB Range + LRU
  (see memory `anyfs_url_load`).
- Cross-origin URLs are rewritten by `applyUrlProxy` ([electron-proxy.ts:42](../ts/packages/core/src/electron-proxy.ts#L42)):
  if `globalThis.__anyfs.urlProxyPrefix` is set (Electron injects `anyfs-url://proxy/?u=` via preload,
  [preload.ts:54-56](../ts/examples/electron-demo/src/preload.ts#L54-L56)), the http(s) URL is rewritten to
  `anyfs-url://proxy/?u=<encoded>`, which main's `handleAnyfsUrlRequest` ([main.ts:133](../ts/examples/electron-demo/src/main.ts#L133))
  fetches in the main process via `net.fetch`, bypassing the renderer CORS policy.
- Loopback addresses are not proxied ([electron-proxy.ts:47](../ts/packages/core/src/electron-proxy.ts#L47)).
- A plain browser (no shell) leaves the prefix unset → URLs go out verbatim (relying on upstream CORS).
- How the prefix reaches the worker: the renderer reads it from `__anyfs`, puts it in the boot message, and the
  worker writes it onto its own `globalThis` via `setUrlProxyPrefix` ([worker.ts:102](../ts/packages/core/src/worker.ts#L102)).

**Mnemonic: native URL = a per-disk HTTP proxy thread in main + QEMU curl; wasm URL = URLFS inside the worker
+ optional anyfs-url:// rewrite. Don't mix them up.**

---

## 7. Partition selection → final mount (DiskView)

File: [DiskView.tsx](../ts/examples/vite-demo/src/components/DiskView.tsx), consumes `useAnyfsDisk()`
(`session/mountPath/status/step/error`).

- **whole disk** (no partitions, autoMount already did `enter(0)`): the provider has set `mountPath` →
  render `DownloadingFileTree` directly ([L116-119](../ts/examples/vite-demo/src/components/DiskView.tsx#L116-L119)).
- **has partitions**:
  - `selectedPart === null` → `listParts()` renders the partition list; clicking → `setSelectedPart(p.index)`
    ([L137-149](../ts/examples/vite-demo/src/components/DiskView.tsx#L137-L149)).
  - `selectedPart !== null` → an effect calls `session.enter(selectedPart)` to get `manualMount`
    → render the file tree ([L77-93](../ts/examples/vite-demo/src/components/DiskView.tsx#L77-L93), [L177](../ts/examples/vite-demo/src/components/DiskView.tsx#L177)).
- File reads all go through `session.openReadable(path)` (base class, [session-base.ts:60](../ts/packages/core/src/session-base.ts#L60)):
  `statFollow` for the size + a streaming `readFd` fd. ⚠️ It must be `statFollow` (follows symlinks); using lstat
  truncates the download stream by the link-target length (see memory `openreadable_needs_stat_follow`).

---

## 8. Electron loading boundaries (artifact view)

- Which renderer the main process loads: `pickRendererDir()` ([main.ts:32-42](../ts/examples/electron-demo/src/main.ts#L32-L42))
  — packaged it's `resources/renderer` (the vite-demo build output), in dev it's the vite dev server.
- addon `.node` resolution: [native-loader.ts](../ts/examples/electron-demo/src/native-loader.ts)
  — packaged `resources/native/anyfs_native.node`, dev `packages/anyfs-native/build/Release/`.
- COOP/COEP headers: injected by the custom `anyfs://` scheme (needed for SAB + service worker);
  `file://` can't carry them (see memory `electron_demo`).

---

## 9. "Open failed" triage checklist (locate by this flow)

| symptom | most likely layer | how to confirm |
|---|---|---|
| `native bridge unavailable` | provider native branch → `prewarmNative` returned null | is `getAnyfsNative()` truthy; does main's `anyfs-native:available` return true (i.e. is the addon loadable) |
| `m.<x> is not a function` | main.ts addon call name vs binding.cc export name drift | `node -e "Object.keys(require('.../anyfs_native.node'))"` and diff against main.ts call names (§5.1) |
| `native backend cannot mount a File object` | source kind doesn't match mode (file+native) | check whether disableNative took effect (needs reload), and whether FilePicker produced `{kind:'file'}` |
| `attachPath not supported` | wasm got a `{kind:'path'}` | same as above, mode/kind mismatch |
| URL open fails (native) | the per-disk proxy thread's HEAD probe failed | check main-process `[http-proxy-worker]` logs ([http-proxy-worker.ts:48](../ts/examples/electron-demo/src/http-proxy-worker.ts#L48)) |
| URL open fails (wasm/browser) | CORS / missing Range | `probeUrlAhead` already surfaces it in FilePicker; under Electron confirm the `anyfs-url://` handler is live |
| toggled disableNative, nothing changed | mode is snapshotted at mount | **reload the page** ([Settings.tsx:162](../ts/examples/vite-demo/src/Settings.tsx#L162) copy) |
| can't enter a partition (e.g. ext4 rc=-74) | an LKL kernel mount/fixture issue, unrelated to transport | if native and wasm behave the same, the transport layer is ruled out |

---

## 10. Consolidated list of "forks / traps" (days of debugging, distilled)

1. **mode snapshot vs nativeMode live** (§1.3, §2): the provider fixes mode at mount; FilePicker reads the live
   setting every render. Toggling without reloading → the two disagree → file+native / path+wasm mismatch throws.
2. **Three naming contracts** (§5.1): IPC channel / bridge method / addon export, three independent things; when
   renaming, touch only the layer that should change. Addon export drift is **not** caught by TS — that was the 2026-05-30 bug.
3. **URL has two completely different implementations** (§5.2 vs §6.4): native = a proxy thread in main, wasm = URLFS in the worker.
4. **Kernel sharing is the opposite** (§5.3 vs §6.3): native is a process-wide singleton (never halts), wasm is per-worker (dispose halts).
5. **Whole-disk vs partition are two mount entry points** (§4.2, §7): 0 partitions → provider autoMount `enter(0)`; with partitions → DiskView `enter(selectedPart)`.
6. **statFollow must not be replaced by lstat** (§7): otherwise openReadable's stream is truncated by the symlink length.
