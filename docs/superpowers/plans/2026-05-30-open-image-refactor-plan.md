# Open-Image Flow Refactor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename `{kind:'file'}`→`{kind:'blob'}` throughout the codebase, add a `createSession()` dispatch factory, unify the per-disk HTTP proxy for both native-url and wasm-path, add whole-window drag-drop with Electron native path resolution, always show the partition picker with a #0 whole-disk entry, restart Electron on disableNative toggle, and convert the native addon to non-blocking AsyncWorker ops.

**Spec:** [`docs/superpowers/specs/2026-05-30-open-image-refactor-design.md`](../specs/2026-05-30-open-image-refactor-design.md)

**Architecture:** Three session implementations (NativeSession / WasmSession / NodeWasmSession) kept separate, each with a single transport. A pure `createSession(env, caps)` factory in `@anyfs/core` selects the backend and declares which source kinds are legal. WasmSession receives a `WasmCaps` flag bag instead of splitting into two classes. The existing per-disk HTTP proxy worker is unified under `anyfs-native:startProxy` (serving both remote URLs and local files). Native addon ops are wrapped in `Napi::AsyncWorker` with C++-layer mutex serialization.

**Tech Stack:** TypeScript, React, Electron 42, Node N-API (C++ binding.cc), emscripten/ASYNCIFY (wasm), libuv thread pool

---

## File map

| File | Change |
|---|---|
| `ts/packages/core/src/types.ts` | Rename kind `'file'` → `'blob'`, field `file` → `blob` |
| `ts/packages/core/src/session.ts` | Rename `attachFile` → `attachBlob` in `AnyfsSession` interface |
| `ts/packages/core/src/session-base.ts` | Rename abstract `attachFile` → `attachBlob` |
| `ts/packages/core/src/wasm-session.ts` | Rename method; add `WasmCaps`; `attachPath` via loopback when electron caps |
| `ts/packages/core/src/native-session.ts` | Rename method; rename `registerUrl`→`startProxy`, `unregisterUrl`→`stopProxy` |
| `ts/packages/core/src/node-wasm-session.ts` | Rename method |
| `ts/packages/core/src/worker.ts` | Rename `attach` arg; forward `urlProxyPrefix` from boot caps |
| `ts/packages/core/src/index.ts` | Export `createSession`, `WasmCaps`; rename `mountFile`→`mountBlob`; `prewarm` takes caps |
| **NEW** `ts/packages/core/src/dispatch.ts` | `WasmCaps` type + `createSession(env, caps)` factory |
| **NEW** `ts/packages/core/test/dispatch.test.ts` | Unit tests for every (env, caps, source) combo |
| `ts/packages/react/src/provider.tsx` | Replace mode logic with factory; remove `autoMount`/`forceMode`; rename attach |
| `ts/examples/vite-demo/src/App.tsx` | Remove autoMount/forceMode; add document-level drag-drop; add `DropOverlay` |
| **NEW** `ts/examples/vite-demo/src/components/DropOverlay.tsx` | Full-window translucent drag overlay |
| `ts/examples/vite-demo/src/components/FilePicker.tsx` | Rename kind; add Electron path resolution; reduce card-level drag handlers |
| `ts/examples/vite-demo/src/components/DiskView.tsx` | Always show partition picker; add #0 whole-disk entry |
| `ts/examples/vite-demo/src/Settings.tsx` | Two-phase confirm flow for disableNative toggle |
| `ts/examples/electron-demo/src/main.ts` | Rename IPC handlers; unified `startProxy`/`stopProxy`; `settings:relaunch`; async awaits (Phase G) |
| `ts/examples/electron-demo/src/preload.ts` | Add `electronFile.pathFor`; rename bridge methods |
| `ts/examples/electron-demo/src/http-proxy-worker.ts` | Extend for `localPath` mode (Range-serving local files) |
| `ts/packages/anyfs-native/src/binding.cc` | Wrap all exports in `Napi::AsyncWorker`; add `std::mutex` serialization queue |

---

### Task 1: Rename `{kind:'file'}` → `{kind:'blob'}` in types and session interface

**Files:**
- Modify: `ts/packages/core/src/types.ts:67-70`
- Modify: `ts/packages/core/src/session.ts:5`

- [ ] **Step 1: Rename in types.ts**

```ts
export type SessionSource =
    | { kind: 'blob'; blob: Blob }
    | { kind: 'url'; url: string; name?: string }
    | { kind: 'path'; path: string; name?: string };
```

- [ ] **Step 2: Rename in session.ts interface**

```ts
export interface AnyfsSession {
    attachBlob(blob: Blob): Promise<void>;
    attachUrl(url: string, name?: string): Promise<void>;
    attachPath(path: string): Promise<void>;
    // ... rest unchanged
}
```

- [ ] **Step 3: Verify typecheck fails on old names**

Run: `cd ts/packages/core && npx tsc --noEmit 2>&1 | head -30`
Expected: errors about `attachFile` not existing on `AnyfsSession`, `kind:'file'` not assignable to `SessionSource`. This confirms we found all call sites before editing them (Tasks 2-3).

- [ ] **Step 4: Commit**

```bash
git add ts/packages/core/src/types.ts ts/packages/core/src/session.ts
git commit -m "refactor(core): rename SessionSource kind 'file'→'blob' and attachFile→attachBlob in interface"
```

---

### Task 2: Rename attachFile → attachBlob in all three session implementations + worker

**Files:**
- Modify: `ts/packages/core/src/session-base.ts:15`
- Modify: `ts/packages/core/src/wasm-session.ts:129-131`
- Modify: `ts/packages/core/src/native-session.ts:82-86`
- Modify: `ts/packages/core/src/node-wasm-session.ts` (find and rename)
- Modify: `ts/packages/core/src/worker.ts:92,165-185`
- Modify: `ts/packages/core/src/index.ts:97-106`

- [ ] **Step 1: Rename abstract method in session-base.ts**

```ts
abstract attachBlob(blob: Blob): Promise<void>;
```

- [ ] **Step 2: Rename in WasmSession**

```ts
async attachBlob(blob: Blob): Promise<void> {
    await this.call('attach', { blob });
}
```

- [ ] **Step 3: Rename in NativeSession**

```ts
async attachBlob(_blob: Blob): Promise<void> {
    throw new Error(
        'NativeSession: attachBlob(Blob) not supported in native mode; use attachPath(string) or fall back to the wasm worker.',
    );
}
```

- [ ] **Step 4: Rename in NodeWasmSession**

Find the current `attachFile` / `attachPath` / `attachUrl` bodies in `node-wasm-session.ts` and rename `attachFile` → `attachBlob` (it should already throw or be unsupported — confirm and rename).

- [ ] **Step 5: Rename worker `attach` op arg**

In `worker.ts` change the `AttachArgs` type and the `attach` handler:

```ts
type AttachArgs = { blob: Blob };

async attach(a: AttachArgs) {
    if (!M) throw new Error('attach: kernel not booted (call boot first)');
    if (diskHandle >= 0) throw new Error('attach: already attached');
    const fsPath = `/work/${a.blob.name || 'image'}`;
    send({ event: 'progress', step: 'attaching disk image' });
    if (!M.WORKERFS) throw new Error('WORKERFS missing');
    M.FS.mkdir('/work');
    M.FS.mount(
        M.WORKERFS,
        { blobs: [{ name: a.blob.name || 'image', data: a.blob }] },
        '/work',
    );
    send({ event: 'progress', step: 'opening disk' });
    diskHandle = await callP('anyfs_ts_session_open_p', ['string', 'number'], [fsPath, 1]);
    if (diskHandle < 0) throw new Error(`anyfs_ts_session_open failed: ${diskHandle}`);
    return { diskHandle };
},
```

Note: the `mount` back-compat op also changes its arg type — `MountArgs = BootArgs & AttachArgs`.

- [ ] **Step 6: Rename public mountFile → mountBlob in index.ts**

```ts
export async function mountBlob(blob: Blob, opts: BrowserMountOpts): Promise<WasmSession> {
    const session = await prewarm(opts);
    try {
        await session.attachBlob(blob);
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}
```

- [ ] **Step 7: Typecheck — should be clean now**

Run: `cd ts/packages/core && npx tsc --noEmit`
Expected: PASS, no errors.

- [ ] **Step 8: Commit**

```bash
git add ts/packages/core/src/session-base.ts ts/packages/core/src/wasm-session.ts \
        ts/packages/core/src/native-session.ts ts/packages/core/src/node-wasm-session.ts \
        ts/packages/core/src/worker.ts ts/packages/core/src/index.ts
git commit -m "refactor(core): rename attachFile→attachBlob in all session impls + worker"
```

---

### Task 3: Update provider and FilePicker call sites for blob rename

**Files:**
- Modify: `ts/packages/react/src/provider.tsx` (all `attachFile` / `kind:'file'` references)
- Modify: `ts/examples/vite-demo/src/components/FilePicker.tsx` (all `kind:'file'` / `acceptFile` references)

Provider changes:
- `source.kind === 'file'` → `source.kind === 'blob'`
- `source.file` → `source.blob`
- `session.attachFile(source.file)` → `session.attachBlob(source.blob)`
- Error message: `'native backend cannot mount a File object'` stays (this is user-facing, blob is an implementation detail)

FilePicker changes:
- `onSource({ kind: 'file', file })` → `onSource({ kind: 'blob', blob: file })`
- `acceptFile` → `acceptBlob`
- All `f.kind === 'file'` / `item.kind !== 'file'` stay as-is (those are `DataTransferItem.kind`, not our kind)
- Type imports update.

Run typecheck after: `cd ts/examples/vite-demo && npx tsc --noEmit`.

Commit.

---

### Task 4: Add WasmCaps type and createSession dispatch factory

**Files:**
- Create: `ts/packages/core/src/dispatch.ts`
- Create: `ts/packages/core/test/dispatch.test.ts`

- [ ] **Step 1: Define WasmCaps and createSession in dispatch.ts**

```ts
import type { SessionSource } from './types.js';
import { getAnyfsNative } from './native-session.js';
import type { AnyfsNativeBridge } from './native-session.js';

/** Capabilities passed to WasmSession at construction time. */
export interface WasmCaps {
    /** Set → URLFS rewrites cross-origin http(s) URLs via this prefix. */
    urlProxyPrefix?: string;
    /** Set → attachPath delegates to attachUrl(this URL, name).
     *  The factory pre-starts the main-process proxy (via startProxy IPC)
     *  and stores the ready-to-use loopback URL here. WasmSession does not
     *  own the proxy lifecycle — the caller tears it down on session close. */
    pathLoopbackUrl?: string;
}

export type SessionEnv = 'web' | 'electron' | 'node';

export type SessionBackend = 'native' | 'wasm' | 'node-wasm';

export interface DispatchResult {
    /** Which backend to use. The provider's prewarm step switches on this. */
    backend: SessionBackend;
    /** Which SessionSource.kind values are legal for this backend. */
    allowedKinds: Set<SessionSource['kind']>;
    /** Present when backend === 'native'. The preload-injected bridge
     *  (already available at provider mount time). */
    nativeBridge?: AnyfsNativeBridge;
    /** Present when backend === 'wasm'. Caps to forward to the worker
     *  and boot message. */
    wasmCaps?: WasmCaps;
}

/**
 * Pure factory: pick the right backend + declare which source kinds are
 * legal. Does NOT construct a session — construction happens during
 * prewarm (worker creation / addon init are async and heavyweight).
 *
 * Throws if the environment provides no usable backend.
 */
export function createSession(env: SessionEnv, opts?: {
    disableNative?: boolean;
    electronWasmCaps?: WasmCaps;
}): DispatchResult {
    if (env === 'web') {
        return { backend: 'wasm', wasmCaps: {}, allowedKinds: new Set(['blob', 'url']) };
    }

    if (env === 'electron') {
        const nativeBridge = getAnyfsNative();
        if (nativeBridge && !opts?.disableNative) {
            return { backend: 'native', nativeBridge, allowedKinds: new Set(['path', 'url']) };
        }
        // Fall through to wasm. Under electron caps, path is legal (via
        // loopback proxy); blob is not (frontend resolves File→path before
        // producing source).
        const caps = opts?.electronWasmCaps ?? {};
        const kinds = new Set<SessionSource['kind']>(['url']);
        if (caps.pathLoopbackUrl) kinds.add('path');
        return { backend: 'wasm', wasmCaps: caps, allowedKinds: kinds };
    }

    // env === 'node'
    return { backend: 'node-wasm', allowedKinds: new Set(['path']) };
}
```

- [ ] **Step 2: Write unit tests in dispatch.test.ts**

```ts
import { strictEqual, deepStrictEqual, throws } from 'node:assert';
import { createSession } from '../src/dispatch.js';

function test(name: string, fn: () => void) {
    try { fn(); console.log(`  ✓ ${name}`); }
    catch (e) { console.error(`  ✗ ${name}: ${(e as Error).message}`); process.exitCode = 1; }
}

// web: wasm backend, blob + url only
test('web → wasm, allowed blob+url', () => {
    const r = createSession('web');
    strictEqual(r.backend, 'wasm');
    deepStrictEqual(r.allowedKinds, new Set(['blob', 'url']));
});

// web: no native bridge
test('web → nativeBridge is undefined', () => {
    const r = createSession('web');
    strictEqual(r.nativeBridge, undefined);
});

console.log('\nDispatch tests complete');
```

- [ ] **Step 3: Run tests**

Run: `node --import tsx ts/packages/core/test/dispatch.test.ts`
Expected: all tests pass.

- [ ] **Step 4: Export from index.ts**

Add to `ts/packages/core/src/index.ts`:
```ts
export { createSession } from './dispatch.js';
export type { WasmCaps, SessionEnv, SessionBackend, DispatchResult } from './dispatch.js';
```

- [ ] **Step 5: Commit**

---

### Task 5: Wire createSession into AnyfsProvider, remove forceMode + autoMount

**Files:**
- Modify: `ts/packages/react/src/provider.tsx`

This is the largest single-file change in the plan. The provider's `mode`/`forceMode` logic and the inline kind-validity checks are replaced by one `createSession` call. The `autoMount` prop and its effect branch are removed.

Key changes to provider.tsx:
1. Import `createSession`, `WasmCaps`, `DispatchResult`, `SessionEnv` from `@anyfs/core`.
   Remove the `getAnyfsNative` / `NativeSession` imports (factory handles that).
2. Props: remove `forceMode` and `autoMount`. Replace with `env: SessionEnv` and optional
   `disableNative` / `electronWasmCaps`.
3. `useState` initializer: call `createSession(env, {disableNative, electronWasmCaps})` →
   store the `DispatchResult` (`{backend, allowedKinds, nativeBridge?, wasmCaps?}`).
4. `startPrewarm` ref: the two-branch `mode === 'native' ? prewarmNative(...) : prewarm(...)`
   becomes a `switch (dispatch.backend)` — `'native'` calls `prewarmNative(dispatch.nativeBridge!)`,
   `'wasm'` calls `prewarm({..., caps: dispatch.wasmCaps})`, `'node-wasm'` throws in browser.
5. Source effect: before attach, `if (!dispatch.allowedKinds.has(source.kind)) throw new Error(...)`.
6. Remove the `autoMount` branch (lines 298-312 in current code) — the provider only does
   `listParts()` now, never `enter(0)`.
7. `session.attachFile` → `session.attachBlob`, `source.file` → `source.blob`,
   `source.kind === 'file'` → `source.kind === 'blob'`.

Typecheck after: `cd ts/packages/react && npx tsc --noEmit`.

Commit.

---

### Task 6: Update App.tsx — remove autoMount/forceMode, pass new provider props

**Files:**
- Modify: `ts/examples/vite-demo/src/App.tsx`

- [ ] **Step 1: Remove the settingsDisableNative IIFE (lines 94-105)**

Delete the block that reads `localStorage.getItem('anyfs.settings.v1')` and parses `disableNative`. The provider no longer takes `forceMode` — mode is decided by the factory, and disableNative is wired differently (Task 17).

- [ ] **Step 2: Remove autoMount and forceMode from AnyfsProvider JSX**

```tsx
<AnyfsProvider
    source={source}
    workerUrl={WORKER_URL}
    wasmBaseUrl="/wasm/"
    wasmModuleName="anyfs.mjs"
    mountOpts={{ loglevel: 7 }}
    prewarm
    env="electron"  // or detected from window.anyfsNative presence
    {...(disableNative ? { disableNative: true } : {})}
>
```

At this point we don't have the new disableNative flow wired yet (Task 16). Keep a simpler interim: detect env from `getAnyfsNative()` presence and read disableNative from localStorage like before, but pass it as a simple boolean to the provider instead of the old `forceMode` prop.

- [ ] **Step 3: Typecheck**

Run: `cd ts/examples/vite-demo && npx tsc --noEmit`

- [ ] **Step 4: Commit**

---

### Task 7: Always show partition picker + add #0 whole-disk entry

**Files:**
- Modify: `ts/examples/vite-demo/src/components/DiskView.tsx`

- [ ] **Step 1: Remove the `mountPath && session` whole-disk fast path**

In the current code (~line 115-119), when `mountPath` is set (provider auto-mounted whole disk), DiskView skips the partition picker and renders the file tree directly. Remove this branch — `mountPath` will now always be null after attach (the provider no longer auto-enters).

- [ ] **Step 2: Always show the partition list, with #0 prepended**

In the `selectedPart === null` branch (~line 137), after `listParts()` returns:

```tsx
const parts = await session.listParts();
const entries = [
    { index: 0, fstype: '', label: 'Whole disk', kind: 'disk' as const },
    ...parts,
];
// render entries.map(e => (
//   <li key={e.index} onClick={() => setSelectedPart(e.index)}>
//     {e.index === 0 ? '💿 Whole disk' : `#${e.index} ${e.fstype} ${e.label}`}
//   </li>
// ))
```

- [ ] **Step 3: Update the "no partitions mounted" / mounting state**

The `mountPath && session` check in the render tree (~line 116) becomes: if `manualMount` is set (from `enter(selectedPart)`), show the file tree; otherwise show the partition list. This is structurally the same as current, just without the auto-mount shortcut.

- [ ] **Step 4: Rebuild vite-demo and typecheck**

Run: `cd ts/examples/vite-demo && npx tsc --noEmit && pnpm build`

- [ ] **Step 5: Commit**

---

### Task 8: Add `electronFile.pathFor` to preload

**Files:**
- Modify: `ts/examples/electron-demo/src/preload.ts`

Add after the existing `electronDialog` block (~line 47):

```ts
// Translate a dropped/picked File object to an absolute host path.
// Electron 42 removed File.path; webUtils.getPathForFile is the replacement.
// Only available in Electron — the renderer feature-detects this object.
const electronFile = {
    pathFor: (file: File) => {
        // webUtils is only available in the preload sandbox with sandbox:false
        const { webUtils } = require('electron') as typeof import('electron');
        return webUtils.getPathForFile(file);
    },
};
contextBridge.exposeInMainWorld('electronFile', electronFile);
```

- [ ] **Step 1: Rebuild preload**

Run: `cd ts/examples/electron-demo && node esbuild.main.mjs`

- [ ] **Step 2: Commit**

---

### Task 9: Whole-window drag-drop overlay component

**Files:**
- Create: `ts/examples/vite-demo/src/components/DropOverlay.tsx`
- Modify: `ts/examples/vite-demo/src/App.tsx`

- [ ] **Step 1: Create DropOverlay.tsx**

```tsx
import { useRef, useState } from 'react';
import type { DragEvent as ReactDragEvent } from 'react';

export function DropOverlay({ onDrop }: { onDrop: (files: FileList) => void }) {
    // Full-viewport fixed overlay. We use a ref counter so dragleave from
    // a child doesn't flicker the overlay off, and a state bool for the CSS.
    const depth = useRef(0);
    const [dragging, setDragging] = useState(false);

    const onDragEnter = (e: ReactDragEvent) => {
        e.preventDefault();
        depth.current++;
        if (depth.current === 1) setDragging(true);
    };
    const onDragOver = (e: ReactDragEvent) => {
        e.preventDefault();
        (e.dataTransfer as DataTransfer).dropEffect = 'copy';
    };
    const onDragLeave = (e: ReactDragEvent) => {
        depth.current--;
        if (depth.current <= 0) {
            depth.current = 0;
            setDragging(false);
        }
    };
    const onDropHere = (e: ReactDragEvent) => {
        e.preventDefault();
        depth.current = 0;
        setDragging(false);
        if (e.dataTransfer.files.length > 0) onDrop(e.dataTransfer.files);
    };

    return (
        <div
            className={`fixed inset-0 z-50 flex items-center justify-center transition-opacity duration-150 ${
                dragging
                    ? 'bg-emerald-500/10 opacity-100 pointer-events-auto'
                    : 'pointer-events-none opacity-0'
            }`}
            onDragEnter={onDragEnter}
            onDragOver={onDragOver}
            onDragLeave={onDragLeave}
            onDrop={onDropHere}
        >
            <div className="bg-white dark:bg-zinc-900 border-2 border-dashed border-emerald-400 rounded-2xl px-8 py-6 shadow-2xl text-center">
                <p className="text-emerald-700 dark:text-emerald-400 text-lg font-semibold">
                    Drop image here
                </p>
                <p className="text-zinc-500 text-sm mt-1">
                    .img .iso .qcow2 .vmdk .vdi .vhd .vhdx .dmg
                </p>
            </div>
        </div>
    );
}
```

- [ ] **Step 2: Wire into App.tsx**

Add state: `const [dropFiles, setDropFiles] = useState<FileList | null>(null);`

In the JSX, inside the outermost wrapper (alongside `<DropOverlay onDrop={handleDrop} />`), add a `handleDrop` callback that:
1. If a source is already loaded → set a confirm dialog ("Replace current image?")
2. Otherwise → resolve path via `electronFile.pathFor` if available, then produce `{kind:'blob'}` or `{kind:'path'}` and call `setSource`.

Remove the per-card drag handlers from FilePicker (the `onDragEnter`/`onDragOver`/`onDragLeave`/`onDrop` on the card div) — they're superseded by the window-level overlay.

- [ ] **Step 3: Typecheck + build**

Run: `cd ts/examples/vite-demo && npx tsc --noEmit && pnpm build`

- [ ] **Step 4: Commit**

---

### Task 10: Electron path resolution in FilePicker

**Files:**
- Modify: `ts/examples/vite-demo/src/components/FilePicker.tsx`

The logic: before calling `onSource`, if `window.electronFile?.pathFor` exists (Electron preload), call it to translate the `File` into an absolute path string. Then produce `{kind:'path'}` instead of `{kind:'blob'}`.

This affects three code paths:
1. `acceptBlob` (FSA picker / legacy input)
2. Drag-drop `onDrop` (now in App.tsx or FilePicker)
3. Reopen from recents (if the recent is a file handle)

Add a helper:

```ts
async function fileToSource(file: File): Promise<SessionSource> {
    const ef = (window as any).electronFile as { pathFor?: (f: File) => Promise<string> } | undefined;
    if (ef?.pathFor) {
        try {
            const p = await ef.pathFor(file);
            const name = sourceName({ kind: 'path', path: p });
            return { kind: 'path', path: p, name };
        } catch {
            // pathFor failed (e.g. sandboxed renderer) — fall through to blob
        }
    }
    return { kind: 'blob', blob: file };
}
```

Replace every `onSource({ kind: 'blob', blob: file })` with `onSource(await fileToSource(file))`.

- [ ] **Step 1: Typecheck + build**

Run: `cd ts/examples/vite-demo && npx tsc --noEmit && pnpm build`

- [ ] **Step 2: Commit**

---

### Task 11: Rename IPC `registerUrl`/`unregisterUrl` → `startProxy`/`stopProxy` in main.ts

**Files:**
- Modify: `ts/examples/electron-demo/src/main.ts:438-479`

This is a rename with no functional change yet (the local-file extension comes in Task 12). Rename:
- IPC channel strings: `anyfs-native:registerUrl` → `anyfs-native:startProxy`, `anyfs-native:unregisterUrl` → `anyfs-native:stopProxy`
- Internal map: `diskProxies` stays
- Payload stays `{ upstreamUrl }` for now

```ts
ipcMain.handle('anyfs-native:startProxy', async (_event, payload: { upstreamUrl: string }) => {
    // ... same as before but with the new channel name
});
ipcMain.handle('anyfs-native:stopProxy', async (_event, id: string) => {
    // ... same as before
});
```

Rebuild main: `cd ts/examples/electron-demo && node esbuild.main.mjs`.

Commit.

---

### Task 12: Extend http-proxy-worker for local file Range-serving

**Files:**
- Modify: `ts/examples/electron-demo/src/http-proxy-worker.ts`

The worker currently only handles `{ upstreamUrl }`. Extend to handle `{ localPath }`:

```ts
const { upstreamUrl, localPath } = workerData as { upstreamUrl?: string; localPath?: string };
```

When `localPath` is set:
1. Stat the file to get `contentLength` (HEAD probe equivalent).
2. On GET: open a `fs.createReadStream` with `{ start, end }` from the Range header.
3. Return appropriate 206/200 status + Content-Range / Content-Length headers.

The simplest correct implementation — `createReadStream` accepts a path directly,
no need to open/fstat:

```ts
import { statSync } from 'node:fs';
import { createReadStream } from 'node:fs';

// In the server handler for GET, when localPath is set:
const total = statSync(localPath).size;
const { range } = req.headers;

if (range) {
    const m = /^bytes=(\d+)-(\d*)$/.exec(range);
    if (m) {
        const start = parseInt(m[1], 10);
        const end = m[2] ? parseInt(m[2], 10) : total - 1;
        if (start >= total) {
            res.writeHead(416, { 'Content-Range': `bytes */${total}` });
            res.end();
            return;
        }
        res.writeHead(206, {
            'Content-Range': `bytes ${start}-${end}/${total}`,
            'Content-Length': String(end - start + 1),
            'Accept-Ranges': 'bytes',
        });
        createReadStream(localPath, { start, end }).pipe(res);
        return;
    }
}
res.writeHead(200, {
    'Content-Length': String(total),
    'Accept-Ranges': 'bytes',
});
createReadStream(localPath).pipe(res);
```

Rebuild main: `cd ts/examples/electron-demo && node esbuild.main.mjs`.

Commit.

---

### Task 13: Extend startProxy IPC to accept localPath, wire WasmSession.attachPath

**Files:**
- Modify: `ts/examples/electron-demo/src/main.ts` (the startProxy handler)
- Modify: `ts/packages/core/src/wasm-session.ts` (attachPath under electron caps)

- [ ] **Step 1: Extend the startProxy IPC handler payload**

Change the handler to accept `{ upstreamUrl?: string; localPath?: string }` (exactly one must be set). Pass the right field to the worker's `workerData`. Everything else (port allocation, token generation) stays the same.

- [ ] **Step 2: Wire WasmSession.attachPath**

In `wasm-session.ts`, `attachPath` delegates to `attachUrl` using the pre-built loopback URL from caps:

```ts
async attachPath(path: string): Promise<void> {
    if (!this.caps.pathLoopbackUrl) {
        throw new Error('WasmSession: attachPath requires electron caps (pathLoopbackUrl)');
    }
    const name = path.split('/').pop() || 'image';
    await this.attachUrl(this.caps.pathLoopbackUrl, name);
}
```

The factory (Task 4) starts the proxy via `startProxy` IPC, gets the URL, and stores it in
`caps.pathLoopbackUrl`. WasmSession does not own the proxy lifecycle — the caller
tears it down on `session.close()`.

- [ ] **Step 3: Rebuild all**

```bash
cd ts/examples/electron-demo && node esbuild.main.mjs
cd ts/packages/core && npx tsc --noEmit
cd ts/examples/vite-demo && pnpm build
```

- [ ] **Step 4: Commit**

---

### Task 14: Update preload and NativeSession bridge for startProxy/stopProxy rename

**Files:**
- Modify: `ts/examples/electron-demo/src/preload.ts:70-75`
- Modify: `ts/packages/core/src/native-session.ts:13-14,88-101`

- [ ] **Step 1: Rename in preload bridge**

```ts
startProxy: (payload: { upstreamUrl?: string; localPath?: string }) =>
    ipcRenderer.invoke('anyfs-native:startProxy', payload) as Promise<{
        proxyUrl: string;
        id: string;
    }>,
stopProxy: (id: string) =>
    ipcRenderer.invoke('anyfs-native:stopProxy', id) as Promise<void>,
```

- [ ] **Step 2: Rename in AnyfsNativeBridge interface + NativeSession calls**

In `native-session.ts`:
- `registerUrl` → `startProxy`
- `unregisterUrl` → `stopProxy`
- Update NativeSession.attachUrl to call `bridge.startProxy({ upstreamUrl: url })`
- Update NativeSession._dispose to call `bridge.stopProxy(id)`

- [ ] **Step 3: Rebuild**

```bash
cd ts/examples/electron-demo && node esbuild.main.mjs
cd ts/packages/core && npx tsc --noEmit
```

- [ ] **Step 4: Commit**

---

### Task 15: Remove mountWhole from preload + bridge interface

**Files:**
- Modify: `ts/examples/electron-demo/src/preload.ts:85-86`
- Modify: `ts/packages/core/src/native-session.ts:13`

`mountWhole` was deleted from the addon in the session-API refactor (commit ce451cb). The IPC handler was already dropped in main.ts (the earlier fix this session). Now clean up the dead surface in preload and the bridge type:

Remove these lines:
- preload: `mountWhole: (h, fstype, flags) => ipcRenderer.invoke(...)`
- native-session.ts `AnyfsNativeBridge`: `mountWhole(...): Promise<string>`

No consumer calls these — this is dead-code removal.

Commit.

---

### Task 16: Settings disableNative confirm dialog (two-phase toggle)

**Files:**
- Modify: `ts/examples/vite-demo/src/Settings.tsx:159-166`

Replace the current direct `onChange` on the disableNative Toggle with a two-phase flow:

```tsx
// In SettingsDialog, near the disableNative Toggle:
const [pendingDisableNative, setPendingDisableNative] = useState<boolean | null>(null);
const [showRestartConfirm, setShowRestartConfirm] = useState(false);

{/* Toggle */}
<Toggle
    label="Disable native module"
    description="..."
    checked={pendingDisableNative ?? settings.disableNative}
    onChange={(v) => {
        setPendingDisableNative(v);
        setShowRestartConfirm(true);
    }}
/>

{/* Confirm dialog */}
{showRestartConfirm && (
    <div className="fixed inset-0 z-[60] flex items-center justify-center bg-black/60"
         onClick={() => { setShowRestartConfirm(false); setPendingDisableNative(null); }}>
        <div className="bg-white dark:bg-zinc-900 border border-zinc-300 dark:border-zinc-700 rounded-lg p-6 max-w-sm mx-4 shadow-xl"
             onClick={e => e.stopPropagation()}>
            <p className="text-zinc-900 dark:text-zinc-100 text-sm font-medium">
                Changing this requires restarting the app. Restart now?
            </p>
            <div className="flex gap-3 mt-4 justify-end">
                <button className="px-3 py-1.5 text-sm text-zinc-600 dark:text-zinc-400 hover:text-zinc-900"
                        onClick={() => { setShowRestartConfirm(false); setPendingDisableNative(null); }}>
                    Cancel
                </button>
                <button className="px-3 py-1.5 text-sm bg-emerald-600 text-white rounded-md hover:bg-emerald-700"
                        onClick={() => {
                            // Commit the setting
                            update('disableNative', pendingDisableNative!);
                            // Persist immediately (the useEffect will fire, but we
                            // need the write to land before the process exits)
                            try {
                                const current = JSON.parse(localStorage.getItem('anyfs.settings.v1') ?? '{}');
                                current.disableNative = pendingDisableNative!;
                                localStorage.setItem('anyfs.settings.v1', JSON.stringify(current));
                            } catch {}
                            // Ask main process to relaunch
                            (window as any).electronSettings?.relaunch?.();
                        }}>
                    Restart
                </button>
            </div>
        </div>
    </div>
)}
```

Add `settings:relaunch` IPC to preload:
```ts
const electronSettings = {
    relaunch: () => ipcRenderer.invoke('settings:relaunch'),
};
contextBridge.exposeInMainWorld('electronSettings', electronSettings);
```

Add handler in main.ts:
```ts
ipcMain.handle('settings:relaunch', () => {
    app.relaunch();
    app.exit();
});
```

- [ ] **Step 1: Typecheck + build + rebuild main**

```bash
cd ts/examples/vite-demo && npx tsc --noEmit && pnpm build
cd ts/examples/electron-demo && node esbuild.main.mjs
```

- [ ] **Step 2: Commit**

---

### Task 17: Verification gate — test one AsyncWorker op in binding.cc

**Files:**
- Create: `ts/packages/anyfs-native/test/async-smoke.js`
- Modify: `ts/packages/anyfs-native/src/binding.cc` (one function only)

**This is the blocking prerequisite for the full AsyncWorker conversion (Task 18).** If the kernel panics or the serialization deadlocks, we escalate to a single-threaded worker_thread approach instead of continuing with AsyncWorker.

- [ ] **Step 1: Convert exactly one function as a prototype — `KernelInit`**

In `binding.cc`, create an AsyncWorker subclass:

```cpp
class KernelInitWorker : public Napi::AsyncWorker {
public:
    KernelInitWorker(Napi::Env env, Napi::Promise::Deferred deferred,
                     uint32_t mem_mb, uint32_t loglevel)
        : Napi::AsyncWorker(env),
          deferred_(std::move(deferred)),
          mem_mb_(mem_mb),
          loglevel_(loglevel),
          rc_(-1) {}

    void Execute() override {
        rc_ = anyfs_ts_kernel_init(mem_mb_, loglevel_);
    }

    void OnOK() override {
        deferred_.Resolve(Napi::Number::New(Env(), rc_));
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    uint32_t mem_mb_;
    uint32_t loglevel_;
    int rc_;
};
```

Change `KernelInit` to return a Promise:
```cpp
Napi::Value KernelInitAsync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    uint32_t mem = info[0].As<Napi::Number>().Uint32Value();
    uint32_t lvl = info[1].As<Napi::Number>().Uint32Value();
    auto deferred = Napi::Promise::Deferred::New(env);
    auto promise = deferred.Promise();           // save promise before moving deferred
    auto* worker = new KernelInitWorker(env, std::move(deferred), mem, lvl);
    worker->Queue();
    return promise;
}
```

Register the export as `kernelInit` (same name, now async).

- [ ] **Step 2: Add mutex for serialization**

```cpp
#include <mutex>
#include <queue>
#include <condition_variable>

static std::mutex g_op_mutex;
```

In `KernelInitWorker::Execute()`:
```cpp
void Execute() override {
    std::lock_guard<std::mutex> lock(g_op_mutex);
    rc_ = anyfs_ts_kernel_init(mem_mb_, loglevel_);
}
```

- [ ] **Step 3: Rebuild addon + run async smoke test**

```bash
cd ts/packages/anyfs-native && npx node-gyp rebuild
```

Create `test/async-smoke.js`:
```js
const addon = require('../build/Release/anyfs_native.node');

async function main() {
    console.log('1. kernelInit (async)...');
    const rc = await addon.kernelInit(256, 0);
    console.log('   rc =', rc);
    if (rc !== 0) throw new Error(`init failed: ${rc}`);

    // The rest of the ops are still sync for now; test that the mutex
    // doesn't deadlock by running init (async) then sessionOpen (sync)
    console.log('2. sessionOpen (sync, should not deadlock)...');
    const h = addon.sessionOpen('/home/kosaka/anyfs-reader/ts/examples/vite-demo/public/disks/multi.img', 1);
    console.log('   handle =', h);
    if (h < 0) throw new Error(`sessionOpen failed: ${h}`);

    addon.sessionClose(h);
    addon.kernelHalt();
    console.log('PASS: async init + sync op did not deadlock');
}

main().catch(e => { console.error(e); process.exit(1); });
```

Run: `node ts/packages/anyfs-native/test/async-smoke.js`
Expected: PASS.

- [ ] **Step 4: If this passes, proceed to Task 18. If the kernel panics or deadlocks, stop — escalate to worker_thread approach.**

- [ ] **Step 5: Commit**

---

### Task 18: Convert all addon exports to AsyncWorker

**Files:**
- Modify: `ts/packages/anyfs-native/src/binding.cc`

**Only proceed if Task 17 passed.**

Apply the same pattern from Task 17 to every export. Two categories:

**Simple numeric-return ops** (`sessionOpen`, `sessionClose`, `fileOpen`, `fileClose`, `pread`):
Create an AsyncWorker subclass per op (or a templated/generic one that takes a `std::function<int()>`).

**String/buffer-return ops** (`sessionListJson`, `sessionMetaJson`, `sessionEnter`, `readdirJson`, `lstatJson`, `statJson`, `readlink`, `realpath`, `readKernelFile`):
AsyncWorker writes the result string into a member `std::string`, `OnOK` resolves with `Napi::String::New`.

Each `Execute()` body wraps the C call in `std::lock_guard<std::mutex> lock(g_op_mutex)`.

Register all exports under their current names (the JS export names don't change — only the return type changes from value to Promise).

- [ ] **Step 1: Build**

```bash
cd ts/packages/anyfs-native && npx node-gyp rebuild
```

- [ ] **Step 2: Run the existing native-session test with await adaptation**

```bash
IMAGE=/home/kosaka/anyfs-reader/ts/examples/vite-demo/public/disks/multi.img \
  node --input-type=module -e "
import { createRequire } from 'module';
const a = createRequire(import.meta.url)('../packages/anyfs-native/build/Release/anyfs_native.node');
const rc = await a.kernelInit(256, 0); console.log('init', rc);
const h = await a.sessionOpen('/home/kosaka/anyfs-reader/ts/examples/vite-demo/public/disks/multi.img', 1);
console.log('open', h);
const parts = JSON.parse(await a.sessionListJson(h));
console.log('parts', parts.length);
const mp = await a.sessionEnter(h, 2, 0);
console.log('enter', mp);
const entries = JSON.parse(await a.readdirJson(mp));
console.log('readdir', entries.length);
await a.sessionClose(h);
await a.kernelHalt();
console.log('PASS');
"
```

Expected: PASS with no kernel panics.

- [ ] **Step 3: Run the concurrent-op stress test**

Two ops submitted simultaneously — the mutex should serialize them:

```bash
node --input-type=module -e "
const a = require('../packages/anyfs-native/build/Release/anyfs_native.node');
await a.kernelInit(256, 0);
const h = await a.sessionOpen('/home/kosaka/anyfs-reader/ts/examples/vite-demo/public/disks/multi.img', 1);
// Fire two ops concurrently
const [p1, p2] = await Promise.all([
    a.sessionListJson(h).then(j => JSON.parse(j).length),
    a.sessionMetaJson(h).then(j => JSON.parse(j).logical_size),
]);
console.log('parts', p1, 'size', p2);
await a.sessionClose(h);
await a.kernelHalt();
console.log('PASS: concurrent ops serialized correctly');
"
```

Expected: PASS.

- [ ] **Step 4: Commit**

---

### Task 19: Update main.ts AnyfsNativeModule type + all IPC handlers for async

**Files:**
- Modify: `ts/examples/electron-demo/src/main.ts:366-383,420-528`

Every addon call now returns a Promise. Update the type declaration:

```ts
type AnyfsNativeModule = {
    kernelInit(memMb: number, loglevel: number): Promise<number>;
    kernelHalt(): Promise<number>;
    sessionOpen(imagePath: string, flags: number): Promise<number>;
    sessionClose(h: number): Promise<number>;
    sessionListJson(h: number): Promise<string>;
    sessionMetaJson(h: number): Promise<string>;
    sessionEnter(h: number, part: number, flags: number): Promise<string>;
    readdirJson(path: string): Promise<string>;
    lstatJson(path: string): Promise<string>;
    statJson(path: string): Promise<string>;
    realpath(path: string): Promise<string>;
    readlink(path: string): Promise<string>;
    fileOpen(path: string, flags: number): Promise<number>;
    pread(fd: number, n: number, off: number): Promise<{ rc: number; data: Uint8Array }>;
    fileClose(fd: number): Promise<number>;
};
```

Add `await` to every IPC handler that calls the addon:

```ts
ipcMain.handle('anyfs-native:init', async (_event, memMb: number, loglevel: number) => {
    const m = loadNativeAddon();
    if (!m) throw new Error('anyfs-native addon not loadable');
    if (nativeInitDone) return 0;
    const rc = await m.kernelInit(memMb >>> 0, loglevel >>> 0);
    if (rc === 0) nativeInitDone = true;
    return rc;
});

ipcMain.handle('anyfs-native:diskOpen', async (_event, path: string, flags: number) => {
    const m = loadNativeAddon()!;
    return await m.sessionOpen(path, flags >>> 0);
});
// ... same pattern for every handler
```

The pread handler changes more than the others: the C `anyfs_ts_pread` writes into a buffer,
and the AsyncWorker's `Execute()` mallocs that buffer and calls pread, then `OnOK()`
creates a `Napi::Buffer::Copy` and resolves with `{rc, data}`. The JS-side signature drops
the `buf` parameter (the buffer is allocated + returned by the worker) and becomes
`pread(fd, n, off): Promise<{rc: number; data: Uint8Array}>`:

```ts
ipcMain.handle('anyfs-native:pread', async (_event, fd: number, n: number, off: number) => {
    const m = loadNativeAddon()!;
    return await m.pread(fd, n >>> 0, off);
});
```

- [ ] **Step 1: Rebuild main + verify**

```bash
cd ts/examples/electron-demo && node esbuild.main.mjs
```

- [ ] **Step 2: Run the Linux Electron native smoke**

```bash
cd ts/examples/electron-demo
xvfb-run -a env -u ELECTRON_RUN_AS_NODE \
  ANYFS_NATIVE_SMOKE=1 \
  ANYFS_NATIVE_IMAGE="$(cd ../vite-demo/public/disks && pwd)/multi.img" \
  ANYFS_NATIVE_OUT=/tmp/anyfs-native-smoke-async.json \
  node_modules/.bin/electron . 2>&1 | grep -E "native:smoke|kernelInit|Error|error"
```

Expected: `[native:smoke] wrote disk list ...` with valid JSON output.

- [ ] **Step 3: Commit**

---

### Task 20: End-to-end verification — both modes, all source kinds

- [ ] **Step 1: Rebuild everything from scratch**

```bash
cd ts/packages/core && pnpm build
cd ts/packages/react && pnpm build
cd ts/examples/vite-demo && pnpm build
cd ts/examples/electron-demo && pnpm stage:renderer && node esbuild.main.mjs
```

- [ ] **Step 2: Wasm smoke (Node)**

```bash
timeout 120 node ts/packages/core/test/smoke.node.mjs 2>&1 | grep -E "smoke|OK|FAIL"
```
Expected: `[smoke] OK`

- [ ] **Step 3: Wasm smoke (browser — if CDP available)**

```bash
# Headless browser test if the harness is set up; otherwise skip.
```

- [ ] **Step 4: Linux Electron native smoke (re-run)**

Same command as Task 19 step 2.

- [ ] **Step 5: Verify the new dispatch factory tests still pass**

```bash
node --import tsx ts/packages/core/test/dispatch.test.ts
```

- [ ] **Step 6: Spot-check the built renderer for stale names**

```bash
grep -c "kind.*['\"]file['\"]" ts/examples/vite-demo/dist/assets/index-*.js
# Expected: 0 (or only UI-facing strings like "Open file…", not kind:'file')
grep -c "attachFile" ts/examples/vite-demo/dist/assets/index-*.js
# Expected: 0
```

- [ ] **Step 7: Final commit**

```bash
git add -A
git commit -m "refactor: open-image flow — blob/path terminology, dispatch factory, unified proxy, drag-drop, partition #0, mode-switch restart, non-blocking native addon"
```

---

## Dependency order

```
Task 1 (rename types)
  → Task 2 (rename impls)
    → Task 3 (rename call sites)
      → Task 4 (factory + caps)
        → Task 5 (provider: factory + remove autoMount)
          → Task 6 (App: remove forceMode/autoMount)
            → Task 7 (partition page #0)
            → Task 8 (preload pathFor)
              → Task 9 (DropOverlay)
                → Task 10 (FilePicker path resolution)
            → Task 11 (rename IPC startProxy/stopProxy)
              → Task 12 (http-proxy-worker localPath)
                → Task 13 (startProxy accepts localPath + WasmSession.attachPath)
                  → Task 14 (preload + NativeSession bridge rename)
                    → Task 15 (remove dead mountWhole)
            → Task 16 (Settings confirm + relaunch)
              → Task 17 (AsyncWorker verification gate)
                → Task 18 (full AsyncWorker conversion)
                  → Task 19 (main.ts async awaits)
                    → Task 20 (e2e verification)
```

Tasks on the same indent level can be parallelized (e.g. 7, 8, 11, 16 after 6; or 12 and 14 after 11).
