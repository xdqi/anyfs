# TS/Frontend Refactoring Design

Date: 2026-05-29
Status: approved

## Goal

Refactor the TypeScript/frontend codebase (`ts/`) for:
1. **Naming alignment** with the new C API (post-C-refactoring: `anyfs_session_*`, `anyfs_kernel_*`)
2. **Deduplication** of `openReadable`, `walk`, `fmtBytes`, and related utilities repeated across implementations
3. **Interface design** — a proper `AnyfsSession` interface and a shared base class for the three session implementations
4. **Component decomposition** — split the ~2100-line `App.tsx` into focused components

Chonky is **not** being replaced.

---

## Module 1: Naming Alignment (C API)

### Type renames

| Current TS type | New TS type | C equivalent |
|---|---|---|
| `DiskHandle` | `SessionHandle` | `AnyfsSession*` (opaque) |
| `DiskMeta` | `SessionMeta` | `AnyfsSessionMeta` |
| `PartInfo` | `SessionPartInfo` | `AnyfsPartInfo` |
| `MountOpts` | `SessionOpts` | `AnyfsKernelOpts` + session flags |
| `EnterOpts` | removed; folded into method params | `uint32_t flags` |
| `DiskSource` | `SessionSource` | — (TS-specific abstraction) |

### Method renames (public API)

| Current | New | Notes |
|---|---|---|
| `disk.enter(part, opts)` | `session.enter(part, flags?)` | `opts.flags` becomes direct arg |
| `disk.mountWhole(fstype)` | **deleted**; use `session.enter(0, fstype)` | C uses `part=0` for whole-disk |
| `disk.diskMeta()` | `session.meta()` | C: `anyfs_session_meta` |
| `disk.listPartitions()` | `session.listParts()` | C: `anyfs_session_list` |
| `disk.dispose()` | `session.close()` | C: `anyfs_session_close` |

LKL-level ops keep their names: `readdir()`, `stat()`, `statFollow()`, `open()`, `read()`, `closeFd()` (renamed from `close()` to avoid collision with session `close()`), `readlink()`, `realpath()`, `readKernelFile()`.

### Class renames

| Current | New | Role |
|---|---|---|
| `AnyfsDisk` (disk.ts) | `DirectSession` | direct emscripten ccall (Node tests) |
| `WorkerAnyfsDisk` | `WorkerSession` | postMessage ↔ Web Worker (browser) |
| `NativeAnyfsDisk` | `NativeSession` | IPC bridge ↔ Electron main process |
| — (new) | `AnyfsSession` | **public interface** that all three implement |
| — (future) | `LocalSession` | Node `fs` module direct access (no LKL) |

### Attach methods

The three open paths remain TS-level abstractions (C has a single `anyfs_session_open(path, flags)`):

| Current | New | Implementation |
|---|---|---|
| `disk.attach(file)` | `session.attachFile(file)` | WorkerSession only |
| `disk.attachUrl(url)` | `session.attachUrl(url)` | WorkerSession / NativeSession |
| `disk.attachPath(path)` | `session.attachPath(path)` | NativeSession only |

---

## Module 2: Base Class

### Motivation

`openReadable()` and `walk()` are implemented identically in three files (disk.ts, worker-client.ts, native-client.ts), each ~50 lines. Both are composed entirely from lower-level primitives (`stat`, `open`, `read`, `closeFd`, `readdir`) that do not depend on the communication mechanism.

### Design

```
                        ┌─────────────────────────────────┐
                        │       AnyfsSessionBase           │
                        │       (abstract class)            │
                        │                                   │
                        │  Implemented (shared):            │
                        │   + openReadable(path, opts)      │
                        │   + walk(root, chunkSize)          │
                        │   + fd tracking (Set<number>)     │
                        │   + close() lifecycle             │
                        │                                   │
                        │  Abstract (per-backend):          │
                        │   # readdir(path): DirEntry[]     │
                        │   # stat(path): Stat              │
                        │   # statFollow(path): Stat        │
                        │   # openFd(path): number          │
                        │   # readFd(fd, offset, len): buf  │
                        │   # closeFd(fd): void             │
                        │   # enter(part, flags): string    │
                        │   # listParts(): PartInfo[]       │
                        │   # meta(): SessionMeta           │
                        │   # readlink(path): string        │
                        │   # realpath(path): string        │
                        │   # readKernelFile(path): string  │
                        └──────┬──────┬───────────────────┘
              ┌────────────────┐│┌──────────────────┐
              │ WorkerSession  │││ NativeSession    │
              │ (postMessage)   │││ (IPC bridge)     │
              └────────────────┘│└──────────────────┘
                     ┌──────────┴──────┐
                     │ DirectSession   │
                     │ (ccall)         │
                     └─────────────────┘
```

### Interface (public export)

```typescript
export interface AnyfsSession {
  // Session lifecycle
  attachFile(file: File): Promise<void>;
  attachUrl(url: string, name?: string): Promise<void>;
  attachPath(path: string): Promise<void>;
  close(): Promise<void>;

  // Partition / mount
  enter(part: number, flags?: number): Promise<string>;
  listParts(): Promise<SessionPartInfo[]>;
  meta(): Promise<SessionMeta>;

  // Filesystem ops
  readdir(path: string): Promise<DirEntry[]>;
  stat(path: string): Promise<Stat>;
  statFollow(path: string): Promise<Stat>;
  readlink(path: string): Promise<string>;
  realpath(path: string): Promise<string>;
  readKernelFile(path: string, maxBytes?: number): Promise<string>;

  // Low-level fd ops
  openFd(path: string): Promise<number>;
  readFd(fd: number, offset: number, length: number): Promise<Uint8Array>;
  closeFd(fd: number): Promise<void>;

  // Derived (from base class)
  openReadable(path: string, opts?: { chunkSize?: number }): Promise<{ stream: ReadableStream<Uint8Array>; size: number }>;
  walk(root: string, chunkSize?: number): AsyncGenerator<string[]>;

  // Events
  onProgress(cb: (step: string) => void): () => void;
}
```

### Ccalls affected (worker.ts + disk.ts)

All `ccall('anyfs_ts_disk_*')` strings updated to match renamed C glue functions (see Module 5).

---

## Module 3: App.tsx Decomposition

### Before

```
ts/examples/vite-demo/src/App.tsx   (~2100 lines, everything in one file)
```

### After

```
ts/examples/vite-demo/src/
├── App.tsx              ← thin shell: <AnyfsProvider> + top-level state + layout
├── TopBar.tsx           ← breadcrumb nav bar
├── FilePicker.tsx       ← drop zone, RecentsList, file-source buttons
├── DiskView.tsx         ← partition list + mounted file tree
├── PartitionPicker.tsx  ← partition selection UI
├── Dialogs.tsx          ← URL prompt, system-drives picker, confirm dialog, error dialog
├── KernelStatusBar.tsx  ← bottom status bar
├── SupportedFormats.tsx ← image-format + filesystem chip display
├── DownloadStatus.tsx   ← download progress bar
├── AboutDialog.tsx      ← about / licenses modal
├── recents.ts           ← (existing) recent-files management
├── Settings.tsx         ← (existing) settings context + dialog
├── stream-download.ts   ← (existing) Service Worker download
└── sw-download.js       ← (existing) Service Worker
```

Naming convention: React component files use **PascalCase** (`TopBar.tsx`), non-component modules use **kebab-case** (`recents.ts`), consistent with existing `App.tsx` / `Settings.tsx`.

---

## Module 4: Shared Utilities → @anyfs/core

Move pure utility functions into `@anyfs/core` (already React-free):

| Function | Currently in | Moved to |
|---|---|---|
| `fmtBytes(n)` | App.tsx, AnyfsFileBrowser.tsx | `@anyfs/core` (new file `src/format.ts`) |
| `fmtMode(mode)` | AnyfsFileBrowser.tsx (private) | same |
| `fmtTime(sec)` | AnyfsFileBrowser.tsx (private) | same |
| `fmtDev(dev)` | AnyfsFileBrowser.tsx (private) | same |
| `formatSize(n)` | App.tsx (private) | same |
| `splitExt(name)` | AnyfsFileBrowser.tsx (private) | same |

These are pure functions with zero React dependency — `@anyfs/core` is the right home without needing a new `@anyfs/shared` package.

---

## Module 5: C Glue File Renames

File: `ts/native/anyfs_ts.c`

| Current C symbol | New C symbol | Delegates to |
|---|---|---|
| `anyfs_ts_init` | `anyfs_ts_kernel_init` | `anyfs_kernel_init` |
| `anyfs_ts_kernel_halt` | unchanged | `anyfs_kernel_halt` |
| `anyfs_ts_disk_open` | `anyfs_ts_session_open` | `anyfs_session_open` |
| `anyfs_ts_disk_close` | `anyfs_ts_session_close` | `anyfs_session_close` |
| `anyfs_ts_disk_list_json` | `anyfs_ts_session_list_json` | `anyfs_session_list` |
| `anyfs_ts_disk_meta_json` | `anyfs_ts_session_meta_json` | `anyfs_session_meta` |
| `anyfs_ts_disk_enter` | `anyfs_ts_session_enter` | `anyfs_session_enter` |
| `anyfs_ts_mount_whole` | **deleted**; merged into `anyfs_ts_session_enter` (part=0) | `anyfs_session_enter` |

`_p` suffixed asyncify trampolines follow their base names.

LKL-level functions keep the `anyfs_ts_` prefix (they are not session-level):
`anyfs_ts_readdir_json`, `anyfs_ts_lstat_json`, `anyfs_ts_stat_json`, `anyfs_ts_open`, `anyfs_ts_pread`, `anyfs_ts_close`, `anyfs_ts_readlink`, `anyfs_ts_realpath`, `anyfs_ts_read_kernel_file`.

### Corresponding TS ccall strings

In `worker.ts` and `disk.ts`: all `'anyfs_ts_disk_*'` string literals updated to match the renamed C symbols above.

### Corresponding N-API bindings

In `ts/packages/anyfs-native`: C function name references in the addon's `Init` / `napi_create_function` calls updated to match.

---

## What Does NOT Change

- **Chonky** — stays as the file-tree component
- **monorepo topology** — no new packages added (`@anyfs/shared` not created)
- **`@anyfs/react`** — provider/hooks keep their names (`AnyfsProvider`, `useAnyfsDisk` → maybe `useAnyfsSession` — TBD at implementation time)
- **`@anyfs/trees`** — component stays, but `fmtBytes` call sites update to import from `@anyfs/core`
- **Build tooling** — tsup config, tsconfig, pnpm workspace unchanged
- **Tests** — update type/method references but keep test structure

---

## Implementation Order

1. **C glue rename** (`ts/native/anyfs_ts.c`) — bottom of the stack, must be first
2. **Type renames** (`types.ts`) — `SessionHandle`, `SessionMeta`, `SessionPartInfo`, `SessionSource`, `SessionOpts`
3. **Base class + interface** (`AnyfsSessionBase`, `AnyfsSession` interface)
4. **Implementations ported** — `DirectSession`, `WorkerSession`, `NativeSession` onto the base class
5. **Shared utilities** — move `fmtBytes` et al. to `@anyfs/core/src/format.ts`
6. **App.tsx split** — decompose into component files
7. **Update consumers** — `@anyfs/react` provider, `@anyfs/trees` file browser, vite-demo imports
8. **Update tests** — reference changes
9. **Update N-API addon** — if any `anyfs_ts_*` names referenced in the addon source
