# TS/Frontend Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename TS API to match C `anyfs_session_*` conventions, deduplicate `openReadable`/`walk` via a shared abstract base class, define an `AnyfsSession` interface, and split the 2100-line `App.tsx` into focused components.

**Architecture:** Bottom-up refactor. Start with C glue and types (foundation), then introduce the abstract base class + interface, port each session implementation onto it, extract shared utilities, update all consumers, and finally decompose App.tsx. Each task produces a compilable intermediate state.

**Tech Stack:** TypeScript 5.5, React 18, tsup, pnpm monorepo, Chonky file browser, Emscripten (wasm), Node N-API

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `ts/native/anyfs_ts.c` | Modify | C glue: rename `anyfs_ts_disk_*` → `anyfs_ts_session_*`, merge `mount_whole` |
| `ts/packages/core/src/types.ts` | Modify | Rename types: `SessionHandle`, `SessionMeta`, `SessionPartInfo`, `SessionSource`, `SessionOpts` |
| `ts/packages/core/src/format.ts` | Create | Shared pure utils: `fmtBytes`, `fmtMode`, `fmtTime`, `fmtDev`, `formatSize`, `splitExt` |
| `ts/packages/core/src/session-base.ts` | Create | Abstract `AnyfsSessionBase` with `openReadable`/`walk`/fd-tracking/`close()` |
| `ts/packages/core/src/session.ts` | Create | `AnyfsSession` interface export |
| `ts/packages/core/src/node-wasm-session.ts` | Rename from `disk.ts` | `NodeWasmSession extends AnyfsSessionBase` — Node, wasm, direct ccall |
| `ts/packages/core/src/wasm-session.ts` | Rename from `worker-client.ts` | `WasmSession extends AnyfsSessionBase` — Browser/Electron, wasm, Worker postMessage |
| `ts/packages/core/src/native-session.ts` | Rename from `native-client.ts` | `NativeSession extends AnyfsSessionBase` — Node/Electron, native addon |
| `ts/packages/core/src/worker.ts` | Modify | Update ccall strings to new C glue names; `close` → `closeFd` |
| `ts/packages/core/src/index.ts` | Modify | Re-export new names |
| `ts/packages/core/src/boot.ts` | Modify | Update ccall strings, use `NodeWasmSession` |
| `ts/packages/core/src/node.ts` | Modify | Use new names |
| `ts/packages/core/tsup.config.ts` | Modify | Update entry points to renamed files |
| `ts/packages/react/src/provider.tsx` | Modify | Use new type/class names |
| `ts/packages/react/src/use-dir.ts` | Modify | Use `readdir` on `AnyfsSession` |
| `ts/packages/react/src/use-file.ts` | Modify | Use `openFd`/`readFd`/`closeFd` on `AnyfsSession` |
| `ts/packages/trees/src/AnyfsFileBrowser.tsx` | Modify | Use new names, import `splitExt`/`fmtBytes` from `@anyfs/core` |
| `ts/examples/vite-demo/src/App.tsx` | Modify → split | Thin shell after decomposition |
| `ts/examples/vite-demo/src/TopBar.tsx` | Create | Breadcrumb nav bar |
| `ts/examples/vite-demo/src/FilePicker.tsx` | Create | Drop zone, recents, file-source buttons |
| `ts/examples/vite-demo/src/DiskView.tsx` | Create | Partition list + mounted file tree |
| `ts/examples/vite-demo/src/Dialogs.tsx` | Create | URL prompt, system-drives, confirm, error dialogs |
| `ts/examples/vite-demo/src/KernelStatusBar.tsx` | Create | Bottom status bar |
| `ts/examples/vite-demo/src/SupportedFormats.tsx` | Create | Format chips |
| `ts/examples/vite-demo/src/DownloadStatus.tsx` | Create | Download progress bar |
| `ts/examples/vite-demo/src/AboutDialog.tsx` | Create | About/licenses modal |

Tests (update references only):
- `ts/packages/core/test/api.node.mjs`
- `ts/packages/core/test/openReadable.node.mjs`
- `ts/packages/core/test/smoke.native.mjs`
- `ts/packages/core/test/smoke.node.mjs`
- `ts/packages/anyfs-native/test/smoke.mjs`
- `ts/packages/anyfs-native/test/smoke-url.mjs`

---

### Task 1: Rename C glue functions

**Files:**
- Modify: `ts/native/anyfs_ts.c`

- [ ] **Step 1: Rename `anyfs_ts_init` → `anyfs_ts_kernel_init`**

```c
// Line 84: change function name
int anyfs_ts_kernel_init(uint32_t mem_mb, uint32_t loglevel)
```

- [ ] **Step 2: Update the async boot caller to use new name**

```c
// Line 113: inside boot_thread_fn
g_boot_result = anyfs_ts_kernel_init(mem_mb, loglevel);
```

- [ ] **Step 3: Rename `anyfs_ts_disk_open` → `anyfs_ts_session_open`**

```c
// Line 157
int anyfs_ts_session_open(const char* image_path, uint32_t flags)
```

- [ ] **Step 4: Rename `anyfs_ts_disk_close` → `anyfs_ts_session_close`**

```c
// Line 171
int anyfs_ts_session_close(int h)
```

- [ ] **Step 5: Rename `anyfs_ts_disk_list_json` → `anyfs_ts_session_list_json`**

```c
// Line 181
int anyfs_ts_session_list_json(int h, char* buf, size_t cap)
```

- [ ] **Step 6: Rename `anyfs_ts_disk_meta_json` → `anyfs_ts_session_meta_json`**

```c
// Line 219
int anyfs_ts_session_meta_json(int h, char* buf, size_t cap)
```

- [ ] **Step 7: Rename `anyfs_ts_disk_enter` → `anyfs_ts_session_enter` and merge `anyfs_ts_mount_whole`**

Replace both functions with a single `anyfs_ts_session_enter` that delegates to `anyfs_session_enter` for both `part=0` (whole-disk) and `part>=1` (partition):

```c
// Line 236 — replace anyfs_ts_disk_enter + anyfs_ts_mount_whole
int anyfs_ts_session_enter(int h, unsigned int part, uint32_t flags,
                           char* mount_out, size_t mount_cap)
{
    AnyfsSession* d = get_handle(h);
    if (!d) return -1;
    if (mount_cap < ANYFS_LKL_PATH_MAX) return -2;

    char lkl_path[ANYFS_LKL_PATH_MAX];
    int rc = anyfs_session_enter(d, part, flags, lkl_path);
    if (rc != 0) return rc < 0 ? rc : -3;
    snprintf(mount_out, mount_cap, "%s", lkl_path);
    return 0;
}
```

- [ ] **Step 8: Delete the old `anyfs_ts_mount_whole` function** (lines 252-315 in the old file)

- [ ] **Step 9: Regenerate the `_p` trampolines**

Replace the `DEF_P_TRAMP` block with updated names:

```c
#define DEF_P_TRAMP(name, params, call)                                        \
    void anyfs_ts_##name##_p params                                        \
    {                                                                      \
        *out = (int32_t)anyfs_ts_##name call;                          \
    }

DEF_P_TRAMP(session_open, (const char* image_path, uint32_t flags, int32_t* out),
            (image_path, flags))
DEF_P_TRAMP(session_list_json, (int h, char* buf, size_t cap, int32_t* out),
            (h, buf, cap))
DEF_P_TRAMP(session_meta_json, (int h, char* buf, size_t cap, int32_t* out),
            (h, buf, cap))
DEF_P_TRAMP(session_enter,
            (int h, unsigned int part, uint32_t flags, char* mount_out,
             size_t mount_cap, int32_t* out),
            (h, part, flags, mount_out, mount_cap))
DEF_P_TRAMP(readdir_json,
            (const char* path, char* buf, size_t cap, int32_t* out),
            (path, buf, cap))
DEF_P_TRAMP(lstat_json, (const char* path, char* buf, size_t cap, int32_t* out),
            (path, buf, cap))
DEF_P_TRAMP(stat_json, (const char* path, char* buf, size_t cap, int32_t* out),
            (path, buf, cap))
DEF_P_TRAMP(realpath, (const char* path, char* buf, size_t cap, int32_t* out),
            (path, buf, cap))
DEF_P_TRAMP(readlink, (const char* path, char* buf, size_t cap, int32_t* out),
            (path, buf, cap))
DEF_P_TRAMP(read_kernel_file,
            (const char* path, char* buf, size_t cap, int32_t* out),
            (path, buf, cap))
DEF_P_TRAMP(open, (const char* path, int flags, int32_t* out), (path, flags))
DEF_P_TRAMP(close, (int fd, int32_t* out), (fd))

#undef DEF_P_TRAMP
```

- [ ] **Step 10: Update `anyfs_ts_pread_p` comment only** (signature stays the same since it's LKL-level)

- [ ] **Step 11: Verify C file compiles**

Run: `make -C /home/kosaka/anyfs-reader/build-anyfs-wasm anyfs_ts.o 2>&1 | head -20`
(Or equivalent for the current build system — just syntax-check the file.)

- [ ] **Step 12: Commit**

```bash
git add ts/native/anyfs_ts.c
git commit -m "refactor(ts): rename C glue anyfs_ts_disk_* → anyfs_ts_session_*

Merge mount_whole into session_enter (part=0). Regenerate _p trampolines.
LKL-level functions keep anyfs_ts_ prefix.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 1b: Update N-API binding.cc to match renamed C glue

**Files:**
- Modify: `ts/packages/anyfs-native/src/binding.cc`

- [ ] **Step 1: Update `extern "C"` declarations**

Replace the old declarations with renamed symbols:

```cpp
extern "C" {
int anyfs_ts_kernel_init(uint32_t mem_mb, uint32_t loglevel);
int anyfs_ts_kernel_halt(void);

const char* anyfs_get_last_error(void);

int anyfs_ts_session_open(const char* image_path, uint32_t flags);
int anyfs_ts_session_close(int h);
int anyfs_ts_session_list_json(int h, char* buf, size_t cap);
int anyfs_ts_session_meta_json(int h, char* buf, size_t cap);
int anyfs_ts_session_enter(int h, unsigned int part, uint32_t flags,
                           char* mount_out, size_t mount_cap);

int anyfs_ts_readdir_json(const char* path, char* buf, size_t cap);
int anyfs_ts_lstat_json(const char* path, char* buf, size_t cap);
int anyfs_ts_stat_json(const char* path, char* buf, size_t cap);
int anyfs_ts_realpath(const char* path, char* buf, size_t cap);
int anyfs_ts_readlink(const char* path, char* buf, size_t cap);

int anyfs_ts_read_kernel_file(const char* path, char* buf, size_t cap);

int anyfs_ts_open(const char* path, int flags);
int64_t anyfs_ts_pread(int fd, void* buf, uint32_t n, int64_t off);
int anyfs_ts_close(int fd);
}
```

- [ ] **Step 2: Rename `Init_` → `KernelInit` and call `anyfs_ts_kernel_init`**

```cpp
static Napi::Value KernelInit(const Napi::CallbackInfo& info)
{
    uint32_t mem = info[0].As<Napi::Number>().Uint32Value();
    uint32_t lvl = info[1].As<Napi::Number>().Uint32Value();
    return Napi::Number::New(info.Env(), anyfs_ts_kernel_init(mem, lvl));
}
```

- [ ] **Step 3: Rename `DiskOpen` → `SessionOpen` and call `anyfs_ts_session_open`**

```cpp
static Napi::Value SessionOpen(const Napi::CallbackInfo& info)
{
    std::string p = info[0].As<Napi::String>();
    uint32_t fl = info[1].As<Napi::Number>().Uint32Value();
    int rc = anyfs_ts_session_open(p.c_str(), fl);
    if (rc < 0) {
        const char* err = anyfs_get_last_error();
        if (err && *err)
            Napi::Error::New(info.Env(), err).ThrowAsJavaScriptException();
        else
            Napi::Error::New(info.Env(), "sessionOpen failed").ThrowAsJavaScriptException();
        return info.Env().Undefined();
    }
    return Napi::Number::New(info.Env(), rc);
}
```

- [ ] **Step 4: Rename `DiskClose` → `SessionClose` and call `anyfs_ts_session_close`**

```cpp
static Napi::Value SessionClose(const Napi::CallbackInfo& info)
{
    int h = info[0].As<Napi::Number>().Int32Value();
    return Napi::Number::New(info.Env(), anyfs_ts_session_close(h));
}
```

- [ ] **Step 5: Rename `DiskListJson` → `SessionListJson` and call `anyfs_ts_session_list_json`**

```cpp
static Napi::Value SessionListJson(const Napi::CallbackInfo& info)
{
    int h = info[0].As<Napi::Number>().Int32Value();
    return CallOverflowing(info.Env(), "sessionListJson",
                           [h](char* b, size_t c) {
                               return anyfs_ts_session_list_json(h, b, c);
                           });
}
```

- [ ] **Step 6: Rename `DiskMetaJson` → `SessionMetaJson` and call `anyfs_ts_session_meta_json`**

```cpp
static Napi::Value SessionMetaJson(const Napi::CallbackInfo& info)
{
    int h = info[0].As<Napi::Number>().Int32Value();
    return CallOverflowing(info.Env(), "sessionMetaJson",
                           [h](char* b, size_t c) {
                               return anyfs_ts_session_meta_json(h, b, c);
                           });
}
```

- [ ] **Step 7: Merge `DiskEnter` + `MountWhole` → `SessionEnter`**

Delete both old functions and replace with:

```cpp
static Napi::Value SessionEnter(const Napi::CallbackInfo& info)
{
    int h = info[0].As<Napi::Number>().Int32Value();
    uint32_t part = info[1].As<Napi::Number>().Uint32Value();
    uint32_t flags = info[2].As<Napi::Number>().Uint32Value();
    char out[256] = {0};
    int rc = anyfs_ts_session_enter(h, part, flags, out, sizeof(out));
    if (rc != 0) {
        Napi::Error::New(info.Env(),
                         "sessionEnter: rc=" + std::to_string(rc))
            .ThrowAsJavaScriptException();
        return info.Env().Null();
    }
    return Napi::String::New(info.Env(), out);
}
```

- [ ] **Step 8: Update `InitModule` exports to use new JS-facing names**

```cpp
static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
    exports.Set("kernelInit", Napi::Function::New(env, KernelInit));
    exports.Set("kernelHalt", Napi::Function::New(env, KernelHalt));

    exports.Set("sessionOpen", Napi::Function::New(env, SessionOpen));
    exports.Set("sessionClose", Napi::Function::New(env, SessionClose));
    exports.Set("sessionListJson", Napi::Function::New(env, SessionListJson));
    exports.Set("sessionMetaJson", Napi::Function::New(env, SessionMetaJson));
    exports.Set("sessionEnter", Napi::Function::New(env, SessionEnter));

    exports.Set("readKernelFile", Napi::Function::New(env, ReadKernelFile));
    exports.Set("readdirJson", Napi::Function::New(env, ReaddirJson));
    exports.Set("lstatJson", Napi::Function::New(env, LstatJson));
    exports.Set("statJson", Napi::Function::New(env, StatJson));
    exports.Set("realpath", Napi::Function::New(env, Realpath_));
    exports.Set("readlink", Napi::Function::New(env, Readlink_));

    exports.Set("fileOpen", Napi::Function::New(env, FileOpen));
    exports.Set("pread", Napi::Function::New(env, Pread));
    exports.Set("fileClose", Napi::Function::New(env, FileClose));
    return exports;
}
```

- [ ] **Step 9: Commit**

```bash
git add ts/packages/anyfs-native/src/binding.cc
git commit -m "refactor(anyfs-native): rename N-API bindings to match session API

JS exports: init→kernelInit, disk*→session*, mountWhole deleted.
C calls: anyfs_ts_init→anyfs_ts_kernel_init, anyfs_ts_disk_*→anyfs_ts_session_*.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Rename types

**Files:**
- Modify: `ts/packages/core/src/types.ts`

- [ ] **Step 1: Replace entire contents of `types.ts`**

```typescript
export type SessionHandle = number;
export type LklFd = number;

export interface SessionPartInfo {
    slot_id: number;
    parent: number;
    index: number;
    offset: number;
    size: number;
    ptype: string;
    kind: string;
    fstype: string;
    label: string;
    uuid: string;
}

export type EntryKind = 'dir' | 'file' | 'link' | 'other';

export interface DirEntry {
    name: string;
    ino: number;
    kind: EntryKind;
}

export interface Stat {
    ino: number;
    mode: number;
    size: number;
    nlink: number;
    mtime: number;
    atime: number;
    ctime: number;
    kind: EntryKind;
    uid?: number;
    gid?: number;
    dev?: number;
    rdev?: number;
    blksize?: number;
    blocks?: number;
}

export interface SessionOpts {
    /** LKL ram size (MiB). Default 64. */
    memMb?: number;
    /** LKL loglevel (0=silent, 7=debug). Default 0. */
    loglevel?: number;
    /** Force whole-disk mount with this fstype, skipping partition probe. */
    forceFstype?: string;
}

/** C: AnyfsSessionMeta */
export interface SessionMeta {
    /** Total logical (virtual block device) size in bytes. */
    logical_size: number;
    /** Outer partition-table flavour: "gpt", "dos", or "" if no PT detected. */
    pt_type: string;
}

/** What the session can attach to. TS-specific — C has a single
 *  anyfs_session_open(path, flags). */
export type SessionSource =
    | { kind: 'file'; file: File }
    | { kind: 'url'; url: string; name?: string }
    | { kind: 'path'; path: string; name?: string };
```

- [ ] **Step 2: Commit**

```bash
git add ts/packages/core/src/types.ts
git commit -m "refactor(ts): rename types — SessionHandle, SessionMeta, SessionPartInfo, SessionSource, SessionOpts

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Create shared format utilities

**Files:**
- Create: `ts/packages/core/src/format.ts`

- [ ] **Step 1: Create `format.ts`**

```typescript
/** Format byte count as human-readable string. */
export function fmtBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
    if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`;
    return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`;
}

/** Format POSIX mode bits as "drwxr-xr-x (0755)". */
export function fmtMode(mode: number): string {
    const types: Array<[number, string]> = [
        [0o140000, 's'],
        [0o120000, 'l'],
        [0o100000, '-'],
        [0o060000, 'b'],
        [0o040000, 'd'],
        [0o020000, 'c'],
        [0o010000, 'p'],
    ];
    let typeCh = '?';
    for (const [m, ch] of types) {
        if ((mode & 0o170000) === m) {
            typeCh = ch;
            break;
        }
    }
    const perm = (bits: number, special: boolean, specialCh: string) => {
        const r = bits & 4 ? 'r' : '-';
        const w = bits & 2 ? 'w' : '-';
        const x = special ? specialCh : bits & 1 ? 'x' : '-';
        return r + w + x;
    };
    const suid = !!(mode & 0o4000);
    const sgid = !!(mode & 0o2000);
    const sticky = !!(mode & 0o1000);
    const u = perm((mode >> 6) & 7, suid, suid ? (mode & 0o0100 ? 's' : 'S') : '');
    const g = perm((mode >> 3) & 7, sgid, sgid ? (mode & 0o0010 ? 's' : 'S') : '');
    const o = perm(mode & 7, sticky, sticky ? (mode & 0o0001 ? 't' : 'T') : '');
    // Recompute with simpler logic:
    const ur = mode & 0o400 ? 'r' : '-';
    const uw = mode & 0o200 ? 'w' : '-';
    const ux = suid ? (mode & 0o100 ? 's' : 'S') : mode & 0o100 ? 'x' : '-';
    const gr = mode & 0o040 ? 'r' : '-';
    const gw = mode & 0o020 ? 'w' : '-';
    const gx = sgid ? (mode & 0o010 ? 's' : 'S') : mode & 0o010 ? 'x' : '-';
    const or_ = mode & 0o004 ? 'r' : '-';
    const ow = mode & 0o002 ? 'w' : '-';
    const ox = sticky ? (mode & 0o001 ? 't' : 'T') : mode & 0o001 ? 'x' : '-';
    return `${typeCh}${ur}${uw}${ux}${gr}${gw}${gx}${or_}${ow}${ox} (0${(mode & 0o7777).toString(8)})`;
}

/** Format a Unix timestamp (seconds) as human-readable date + epoch. */
export function fmtTime(sec: number): string {
    if (!sec) return '—';
    const d = new Date(sec * 1000);
    return `${d.toISOString().replace('T', ' ').replace(/\.\d+Z$/, ' UTC')} (epoch ${sec})`;
}

/** Format Linux dev_t as "major:minor (raw)". */
export function fmtDev(dev: number): string {
    const major = ((dev >>> 8) & 0xfff) | ((Math.floor(dev / 0x100000000) >>> 0) & 0xfffff000);
    const minor = (dev & 0xff) | ((dev >>> 12) & 0xffffff00);
    return `${major}:${minor} (${dev})`;
}

/** Format a raw number of bytes with adaptive units (used by Recents/disk summary). */
export function formatSize(n: number | undefined): string {
    if (n === undefined || !Number.isFinite(n)) return '';
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let v = n;
    let u = 0;
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024;
        u++;
    }
    return `${v < 10 && u > 0 ? v.toFixed(1) : Math.round(v)} ${units[u]}`;
}

/**
 * Split a filename's extension.
 * Rules:
 *   - no dot → no extension (`""`)
 *   - leading dot (dotfile like `.pwd.lock`) → only split on a *later* dot
 *   - trailing dot → no extension
 */
export function splitExt(name: string): string {
    const i = name.lastIndexOf('.');
    if (i <= 0) return '';
    if (i === name.length - 1) return '';
    return name.substring(i);
}
```

- [ ] **Step 2: Commit**

```bash
git add ts/packages/core/src/format.ts
git commit -m "feat(core): add shared format utilities — fmtBytes, fmtMode, fmtTime, fmtDev, formatSize, splitExt

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Create AnyfsSession interface and abstract base class

**Files:**
- Create: `ts/packages/core/src/session.ts`
- Create: `ts/packages/core/src/session-base.ts`

- [ ] **Step 1: Create `session.ts` — the public interface**

```typescript
import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';

export interface AnyfsSession {
    // ── Lifecycle ──────────────────────────────────────
    attachFile(file: File): Promise<void>;
    attachUrl(url: string, name?: string): Promise<void>;
    attachPath(path: string): Promise<void>;
    close(): Promise<void>;

    // ── Partition / mount ─────────────────────────────
    /** Enter a partition. part=0 mounts the whole disk. */
    enter(part: number, flags?: number): Promise<string>;
    listParts(): Promise<SessionPartInfo[]>;
    meta(): Promise<SessionMeta>;

    // ── Filesystem ops ────────────────────────────────
    readdir(path: string): Promise<DirEntry[]>;
    stat(path: string): Promise<Stat>;
    statFollow(path: string): Promise<Stat>;
    readlink(path: string): Promise<string>;
    realpath(path: string): Promise<string>;
    readKernelFile(path: string, maxBytes?: number): Promise<string>;

    // ── Low-level fd ops ──────────────────────────────
    openFd(path: string): Promise<LklFd>;
    readFd(fd: LklFd, offset: number, length: number): Promise<Uint8Array>;
    closeFd(fd: LklFd): Promise<void>;

    // ── Derived ───────────────────────────────────────
    openReadable(
        path: string,
        opts?: { chunkSize?: number },
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }>;
    walk(root: string, chunkSize?: number): AsyncGenerator<string[]>;

    // ── Events ────────────────────────────────────────
    onProgress(cb: (step: string) => void): () => void;
}
```

- [ ] **Step 2: Create `session-base.ts` — the abstract base class**

```typescript
import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import type { AnyfsSession } from './session.js';

/**
 * Abstract base for all session implementations.
 * Subclasses implement the transport-specific abstract methods.
 * The base provides openReadable(), walk(), fd tracking, and close() lifecycle.
 */
export abstract class AnyfsSessionBase implements AnyfsSession {
    protected disposed = false;
    protected readonly fds = new Set<LklFd>();

    // ── Subclass contract ─────────────────────────────

    abstract attachFile(file: File): Promise<void>;
    abstract attachUrl(url: string, name?: string): Promise<void>;
    abstract attachPath(path: string): Promise<void>;
    abstract enter(part: number, flags?: number): Promise<string>;
    abstract listParts(): Promise<SessionPartInfo[]>;
    abstract meta(): Promise<SessionMeta>;
    abstract readdir(path: string): Promise<DirEntry[]>;
    abstract stat(path: string): Promise<Stat>;
    abstract statFollow(path: string): Promise<Stat>;
    abstract readlink(path: string): Promise<string>;
    abstract realpath(path: string): Promise<string>;
    abstract readKernelFile(path: string, maxBytes?: number): Promise<string>;
    abstract onProgress(cb: (step: string) => void): () => void;

    /** @internal — open a file descriptor (transport-specific). */
    protected abstract _openFdRaw(path: string): Promise<LklFd>;
    /** @internal — read from a file descriptor (transport-specific). */
    protected abstract _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array>;
    /** @internal — close a file descriptor (transport-specific). */
    protected abstract _closeFdRaw(fd: LklFd): Promise<void>;
    /** @internal — release backend resources (transport-specific). */
    protected abstract _dispose(): Promise<void>;

    // ── Public fd ops (with tracking) ─────────────────

    async openFd(path: string): Promise<LklFd> {
        this.check();
        const fd = await this._openFdRaw(path);
        this.fds.add(fd);
        return fd;
    }

    async readFd(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        this.check();
        return this._readFdRaw(fd, offset, length);
    }

    async closeFd(fd: LklFd): Promise<void> {
        this.check();
        this.fds.delete(fd);
        await this._closeFdRaw(fd);
    }

    // ── Shared: openReadable ──────────────────────────

    async openReadable(
        path: string,
        opts: { chunkSize?: number } = {},
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }> {
        const chunkSize = opts.chunkSize ?? 1024 * 1024;
        const st = await this.statFollow(path);
        const total = st.size;
        const fd = await this.openFd(path);
        let offset = 0;
        let closed = false;
        const closeFd = async () => {
            if (closed) return;
            closed = true;
            try { await this.closeFd(fd); } catch { /* best effort */ }
        };
        const self = this;
        const stream = new ReadableStream<Uint8Array>({
            async pull(controller) {
                if (offset >= total) {
                    await closeFd();
                    controller.close();
                    return;
                }
                const want = Math.min(chunkSize, total - offset);
                try {
                    const chunk = await self.readFd(fd, offset, want);
                    if (chunk.length === 0) {
                        await closeFd();
                        controller.close();
                        return;
                    }
                    offset += chunk.length;
                    controller.enqueue(chunk);
                    if (offset >= total) {
                        await closeFd();
                        controller.close();
                    }
                } catch (err) {
                    await closeFd();
                    controller.error(err);
                }
            },
            async cancel() {
                await closeFd();
            },
        });
        return { stream, size: total };
    }

    // ── Shared: walk ──────────────────────────────────

    async *walk(root: string, chunkSize = 1000): AsyncGenerator<string[]> {
        this.check();
        const queue: string[] = [root];
        let chunk: string[] = [];
        while (queue.length) {
            const dir = queue.shift()!;
            let entries: DirEntry[];
            try {
                entries = await this.readdir(dir);
            } catch {
                continue;
            }
            for (const e of entries) {
                const p = dir === '/' ? `/${e.name}` : `${dir}/${e.name}`;
                chunk.push(p);
                if (e.kind === 'dir') queue.push(p);
                if (chunk.length >= chunkSize) {
                    yield chunk;
                    chunk = [];
                }
            }
        }
        if (chunk.length) yield chunk;
    }

    // ── Shared: close lifecycle ───────────────────────

    async close(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        // Best-effort close all tracked fds
        for (const fd of this.fds) {
            try { await this._closeFdRaw(fd); } catch { /* best effort */ }
        }
        this.fds.clear();
        await this._dispose();
    }

    // ── Internal ──────────────────────────────────────

    protected check(): void {
        if (this.disposed) throw new Error('AnyfsSession: already disposed');
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add ts/packages/core/src/session.ts ts/packages/core/src/session-base.ts
git commit -m "feat(core): add AnyfsSession interface + AnyfsSessionBase abstract class

Shared openReadable/walk/fd-tracking/close lifecycle.
Subclasses only implement transport-specific primitives.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Port NodeWasmSession (was `disk.ts`)

**Files:**
- Rename: `ts/packages/core/src/disk.ts` → `ts/packages/core/src/node-wasm-session.ts`
- Modify: full rewrite to extend `AnyfsSessionBase`

- [ ] **Step 1: Read current `disk.ts` and `boot.ts` for ccall patterns**

Current file at `ts/packages/core/src/disk.ts` (257 lines). The new file will keep the core ccall logic but drop `openReadable` and `walk` (now in base).

- [ ] **Step 2: Create `node-wasm-session.ts`**

```typescript
import { AnyfsSessionBase } from './session-base.js';
import type { AnyfsModule } from './module.js';
import type { DirEntry, LklFd, SessionHandle, SessionMeta, SessionPartInfo, Stat } from './types.js';

/** Default initial buffer for JSON syscalls (bytes). Grows on demand. */
const JSON_BUF_INIT = 4096;

export class NodeWasmSession extends AnyfsSessionBase {
    private readonly M: AnyfsModule;
    private h: SessionHandle = -1;

    /** Construct a Node wasm session. Use attachPath() to open a disk image. */
    constructor(M: AnyfsModule) {
        super();
        this.M = M;
    }

    // ── Lifecycle ─────────────────────────────────────

    async attachFile(_file: File): Promise<void> {
        throw new Error('NodeWasmSession: attachFile not available. Use WasmSession for browser File API support.');
    }

    async attachUrl(_url: string, _name?: string): Promise<void> {
        throw new Error('NodeWasmSession: attachUrl not available. Use WasmSession (browser URLFS) or NativeSession (Node http-proxy) instead.');
    }

    /** Open a disk image by filesystem path via NODEFS + anyfs_session_open. */
    async attachPath(fsPath: string): Promise<void> {
        this.check();
        if (this.h >= 0) throw new Error('NodeWasmSession: already attached');
        const h = this.M.ccall(
            'anyfs_ts_session_open',
            'number',
            ['string', 'number'],
            [fsPath, 0],
        ) as number;
        if (h < 0) throw new Error(`session_open(${fsPath}) failed: ${h}`);
        this.h = h;
    }

    // ── Partition / mount ─────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        this.check();
        const cap = 128;
        const buf = this.M._malloc(cap);
        try {
            const rc = this.M.ccall(
                'anyfs_ts_session_enter',
                'number',
                ['number', 'number', 'number', 'number', 'number'],
                [this.h, part, flags, buf, cap],
            ) as number;
            if (rc !== 0) throw new Error(`session_enter failed: rc=${rc}`);
            return this.M.UTF8ToString(buf);
        } finally {
            this.M._free(buf);
        }
    }

    async listParts(): Promise<SessionPartInfo[]> {
        this.check();
        const json = this._callJsonHandle('anyfs_ts_session_list_json');
        return JSON.parse(json) as SessionPartInfo[];
    }

    async meta(): Promise<SessionMeta> {
        this.check();
        const json = this._callJsonHandle('anyfs_ts_session_meta_json');
        return JSON.parse(json) as SessionMeta;
    }

    // ── Filesystem ops ────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        this.check();
        const json = this._callJsonString('anyfs_ts_readdir_json', path);
        return JSON.parse(json) as DirEntry[];
    }

    async stat(path: string): Promise<Stat> {
        this.check();
        const json = this._callJsonString('anyfs_ts_lstat_json', path);
        return JSON.parse(json) as Stat;
    }

    async statFollow(path: string): Promise<Stat> {
        this.check();
        const json = this._callJsonString('anyfs_ts_stat_json', path);
        return JSON.parse(json) as Stat;
    }

    async readlink(path: string): Promise<string> {
        this.check();
        const cap = 4096;
        const buf = this.M._malloc(cap);
        try {
            const n = this.M.ccall(
                'anyfs_ts_readlink_p',
                'number',
                ['string', 'number', 'number'],
                [path, buf, cap],
            ) as number;
            if (n < 0) throw new Error(`readlink rc=${n}`);
            return this.M.UTF8ToString(buf, n);
        } finally {
            this.M._free(buf);
        }
    }

    async realpath(path: string): Promise<string> {
        this.check();
        const cap = 4096;
        const buf = this.M._malloc(cap);
        try {
            const n = this.M.ccall(
                'anyfs_ts_realpath_p',
                'number',
                ['string', 'number', 'number'],
                [path, buf, cap],
            ) as number;
            if (n < 0) throw new Error(`realpath rc=${n}`);
            return this.M.UTF8ToString(buf, n);
        } finally {
            this.M._free(buf);
        }
    }

    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        this.check();
        let cap = 4096;
        for (let i = 0; i < 5; i++) {
            const buf = this.M._malloc(cap);
            try {
                const n = this.M.ccall(
                    'anyfs_ts_read_kernel_file_p',
                    'number',
                    ['string', 'number', 'number'],
                    [path, buf, cap],
                ) as number;
                if (n >= 0) return this.M.UTF8ToString(buf, n);
                const need = -n;
                if (need <= cap) throw new Error(`readKernelFile rc=${n}`);
                cap = Math.max(need + 256, cap * 2);
            } finally {
                this.M._free(buf);
            }
        }
        throw new Error('readKernelFile: buffer too large');
    }

    // ── Events ────────────────────────────────────────

    onProgress(_cb: (step: string) => void): () => void {
        return () => {};
    }

    // ── Internal fd ops ───────────────────────────────

    protected async _openFdRaw(path: string): Promise<LklFd> {
        const fd = this.M.ccall(
            'anyfs_ts_open',
            'number',
            ['string', 'number'],
            [path, 0],
        ) as number;
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        return fd;
    }

    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        const buf = this.M._malloc(length);
        try {
            const got = this.M.ccall(
                'anyfs_ts_pread',
                'number',
                ['number', 'number', 'number', 'bigint'],
                [fd, buf, length, BigInt(offset)],
            ) as unknown as bigint | number;
            const n = typeof got === 'bigint' ? Number(got) : got;
            if (n < 0) throw new Error(`pread failed: ${n}`);
            return new Uint8Array(this.M.HEAPU8.buffer, buf, n).slice();
        } finally {
            this.M._free(buf);
        }
    }

    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        const rc = this.M.ccall('anyfs_ts_close', 'number', ['number'], [fd]) as number;
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    protected async _dispose(): Promise<void> {
        this.M.ccall('anyfs_ts_session_close', 'number', ['number'], [this.h]);
    }

    // ── JSON helpers ──────────────────────────────────

    private _callJsonString(fnName: string, pathArg: string): string {
        let cap = JSON_BUF_INIT;
        while (true) {
            const buf = this.M._malloc(cap);
            try {
                const ret = this.M.ccall(
                    fnName,
                    'number',
                    ['string', 'number', 'number'],
                    [pathArg, buf, cap],
                ) as number;
                if (ret < 0) {
                    cap = Math.max(cap * 2, -ret);
                    continue;
                }
                return this.M.UTF8ToString(buf, ret);
            } finally {
                this.M._free(buf);
            }
        }
    }

    private _callJsonHandle(fnName: string): string {
        let cap = JSON_BUF_INIT;
        while (true) {
            const buf = this.M._malloc(cap);
            try {
                const ret = this.M.ccall(
                    fnName,
                    'number',
                    ['number', 'number', 'number'],
                    [this.h, buf, cap],
                ) as number;
                if (ret < 0) {
                    cap = Math.max(cap * 2, -ret);
                    continue;
                }
                return this.M.UTF8ToString(buf, ret);
            } finally {
                this.M._free(buf);
            }
        }
    }
}
```

- [ ] **Step 3: Delete old `disk.ts`**

```bash
git rm ts/packages/core/src/disk.ts
```

- [ ] **Step 4: Commit**

```bash
git add ts/packages/core/src/node-wasm-session.ts && git rm ts/packages/core/src/disk.ts
git commit -m "refactor(core): rename AnyfsDisk → NodeWasmSession, extend AnyfsSessionBase

Drop openReadable/walk (now in base). Update ccall strings to
anyfs_ts_session_* names. Keep JSON helper pattern.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Port WasmSession (was `worker-client.ts`)

**Files:**
- Rename: `ts/packages/core/src/worker-client.ts` → `ts/packages/core/src/wasm-session.ts`
- Modify: full rewrite to extend `AnyfsSessionBase`

- [ ] **Step 1: Create `wasm-session.ts`**

```typescript
import { AnyfsSessionBase } from './session-base.js';
import type {
    DirEntry,
    LklFd,
    SessionHandle,
    SessionMeta,
    SessionPartInfo,
    SessionSource,
    Stat,
} from './types.js';

type Pending = { res: (v: unknown) => void; rej: (e: Error) => void };

/**
 * Session implementation that proxies all ops to a Web Worker hosting the
 * wasm runtime via postMessage. Mirrors NodeWasmSession's surface.
 */
export class WasmSession extends AnyfsSessionBase {
    private readonly worker: Worker;
    private nextId = 1;
    private readonly pending = new Map<number, Pending>();
    private workerError: Error | null = null;

    /** @internal — constructed by mountFile() in index.ts. */
    constructor(worker: Worker) {
        super();
        this.worker = worker;
        this.worker.addEventListener('message', this._onMessage);
        this.worker.addEventListener('error', this._onError);
    }

    // ── Message plumbing ──────────────────────────────

    private _onMessage = (e: MessageEvent) => {
        const m = e.data as {
            id?: number; ok?: boolean; result?: unknown; error?: string; stack?: string;
            event?: string; message?: string; reason?: string; step?: string;
        };
        if (m.event === 'abort' || m.event === 'host-error' || m.event === 'host-rejection') {
            this.workerError = new Error(`anyfs worker ${m.event}: ${m.message ?? m.reason ?? ''}`);
            for (const p of this.pending.values()) p.rej(this.workerError);
            this.pending.clear();
            return;
        }
        if (m.event === 'stdout' || m.event === 'stderr') {
            const tag = m.event === 'stderr' ? 'anyfs.err' : 'anyfs.out';
            console.log(`[${tag}] ${(m.message ?? '').replace(/\n$/, '')}`);
            return;
        }
        if (m.event === 'progress') {
            console.log(`[anyfs] ${m.step ?? ''}`);
            return;
        }
        if (typeof m.id !== 'number') return;
        const p = this.pending.get(m.id);
        if (!p) return;
        this.pending.delete(m.id);
        if (m.ok) p.res(m.result);
        else p.rej(new Error((m.error ?? 'worker call failed') + (m.stack ? `\n${m.stack}` : '')));
    };

    private _onError = (e: ErrorEvent) => {
        this.workerError = new Error(`anyfs worker error: ${e.message}`);
        for (const p of this.pending.values()) p.rej(this.workerError);
        this.pending.clear();
    };

    private _call<T>(op: string, args: unknown = {}): Promise<T> {
        if (this.disposed) return Promise.reject(new Error('AnyfsSession: already disposed'));
        if (this.workerError) return Promise.reject(this.workerError);
        const id = this.nextId++;
        return new Promise<T>((res, rej) => {
            this.pending.set(id, { res: res as (v: unknown) => void, rej });
            try {
                this.worker.postMessage({ id, op, args });
            } catch (err) {
                this.pending.delete(id);
                rej(err instanceof Error ? err : new Error(String(err)));
            }
        });
    }

    /** @internal — exposed for prewarm boot sequence. */
    callRaw<T>(op: string, args: unknown = {}): Promise<T> {
        return this._call<T>(op, args);
    }

    /** Wait for the worker to signal host-ready. */
    static waitForReady(worker: Worker, timeoutMs = 10000): Promise<void> {
        return new Promise((res, rej) => {
            const t = setTimeout(() => {
                cleanup();
                rej(new Error('anyfs worker did not become ready'));
            }, timeoutMs);
            const onMsg = (e: MessageEvent) => {
                const m = e.data as { event?: string; message?: string };
                if (m.event === 'host-ready') { cleanup(); res(); }
                else if (m.event === 'host-error' || m.event === 'abort') {
                    cleanup();
                    rej(new Error(`anyfs worker boot ${m.event}: ${m.message ?? ''}`));
                }
            };
            const onErr = (ev: ErrorEvent) => {
                cleanup();
                rej(new Error(`anyfs worker error: ${ev.message}`));
            };
            const cleanup = () => {
                clearTimeout(t);
                worker.removeEventListener('message', onMsg);
                worker.removeEventListener('error', onErr);
            };
            worker.addEventListener('message', onMsg);
            worker.addEventListener('error', onErr);
        });
    }

    // ── Lifecycle ─────────────────────────────────────

    async attachFile(file: File): Promise<void> {
        await this._call('attachFile', { file });
    }

    async attachUrl(url: string, name?: string): Promise<void> {
        let fallback = name?.trim() || '';
        if (!fallback) {
            try {
                const u = new URL(url, typeof self !== 'undefined' ? self.location.href : 'http://x/');
                fallback = u.pathname.split('/').filter(Boolean).pop() || 'image';
            } catch { fallback = 'image'; }
        }
        await this._call('attachUrl', { url, name: fallback });
    }

    async attachPath(_path: string): Promise<void> {
        throw new Error('WasmSession: attachPath not yet available. Planned via URLFS + privileged HTTP proxy for Electron.');
    }

    // ── Partition / mount ─────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        return this._call<string>('enter', { part, flags });
    }

    async listParts(): Promise<SessionPartInfo[]> {
        return this._call<SessionPartInfo[]>('listParts');
    }

    async meta(): Promise<SessionMeta> {
        return this._call<SessionMeta>('meta');
    }

    // ── Filesystem ops ────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        return this._call<DirEntry[]>('readdir', { path });
    }

    async stat(path: string): Promise<Stat> {
        return this._call<Stat>('stat', { path });
    }

    async statFollow(path: string): Promise<Stat> {
        return this._call<Stat>('statFollow', { path });
    }

    async readlink(path: string): Promise<string> {
        return this._call<string>('readlink', { path });
    }

    async realpath(path: string): Promise<string> {
        return this._call<string>('realpath', { path });
    }

    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        return this._call<string>('readKernelFile', { path });
    }

    // ── Events ────────────────────────────────────────

    onProgress(cb: (step: string) => void): () => void {
        const handler = (e: MessageEvent) => {
            const m = e.data as { event?: string; step?: string };
            if (m.event === 'progress' && m.step) cb(m.step);
        };
        this.worker.addEventListener('message', handler);
        return () => this.worker.removeEventListener('message', handler);
    }

    // ── Internal fd ops ───────────────────────────────

    protected async _openFdRaw(path: string): Promise<LklFd> {
        const fd = await this._call<number>('openFd', { path });
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        return fd;
    }

    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        return this._call<Uint8Array>('readFd', { fd, offset, length });
    }

    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        const rc = await this._call<number>('closeFd', { fd });
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    protected async _dispose(): Promise<void> {
        try { await this._call<number>('dispose'); } catch { /* best effort */ }
        this.worker.removeEventListener('message', this._onMessage);
        this.worker.removeEventListener('error', this._onError);
        this.worker.terminate();
        this.pending.clear();
    }
}
```

- [ ] **Step 2: Delete old `worker-client.ts`**

```bash
git rm ts/packages/core/src/worker-client.ts
```

- [ ] **Step 3: Commit**

```bash
git add ts/packages/core/src/wasm-session.ts && git rm ts/packages/core/src/worker-client.ts
git commit -m "refactor(core): rename WorkerAnyfsDisk → WasmSession, extend AnyfsSessionBase

Drop openReadable/walk (now in base). Rename ops to match new convention:
open/read/close → openFd/readFd/closeFd. attach → attachFile/attachUrl.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Port NativeSession (was `native-client.ts`)

**Files:**
- Rename: `ts/packages/core/src/native-client.ts` → `ts/packages/core/src/native-session.ts`
- Modify: full rewrite to extend `AnyfsSessionBase`

- [ ] **Step 1: Create `native-session.ts`**

```typescript
import { AnyfsSessionBase } from './session-base.js';
import type {
    DirEntry,
    LklFd,
    SessionHandle,
    SessionMeta,
    SessionPartInfo,
    Stat,
} from './types.js';

/** Shape of the preload-injected bridge. */
export interface AnyfsNativeBridge {
    available(): Promise<boolean>;
    kernelInit(memMb: number, loglevel: number): Promise<number>;
    sessionOpen(path: string, flags: number): Promise<number>;
    sessionClose(h: number): Promise<number>;
    sessionListJson(h: number): Promise<string>;
    sessionMetaJson(h: number): Promise<string>;
    sessionEnter(h: number, part: number, flags: number): Promise<string>;
    readdirJson(path: string): Promise<string>;
    lstatJson(path: string): Promise<string>;
    statJson(path: string): Promise<string>;
    realpath(path: string): Promise<string>;
    readlink(path: string): Promise<string>;
    registerUrl(url: string): Promise<{ proxyUrl: string; id: string }>;
    unregisterUrl(id: string): Promise<void>;
    fileOpen(path: string, flags: number): Promise<number>;
    pread(fd: number, n: number, off: number): Promise<{ rc: number; data: Uint8Array }>;
    fileClose(fd: number): Promise<number>;
}

export function getAnyfsNative(): AnyfsNativeBridge | null {
    try {
        const g = (globalThis as unknown as { anyfsNative?: AnyfsNativeBridge }).anyfsNative;
        if (g && typeof g.kernelInit === 'function') return g;
    } catch { /* sandboxed contextBridge */ }
    return null;
}

export class NativeSession extends AnyfsSessionBase {
    private readonly bridge: AnyfsNativeBridge;
    private handle: SessionHandle = -1;
    private proxyId: string | null = null;
    private opChain: Promise<unknown> = Promise.resolve();

    constructor(bridge: AnyfsNativeBridge) {
        super();
        this.bridge = bridge;
    }

    private chain<T>(fn: () => Promise<T>): Promise<T> {
        if (this.disposed) return Promise.reject(new Error('AnyfsSession: already disposed'));
        const next = this.opChain.then(fn, fn);
        this.opChain = next.catch(() => undefined);
        return next;
    }

    /** Boot the addon's kernel (idempotent). */
    async boot(memMb: number, loglevel: number): Promise<void> {
        const rc = await this.bridge.kernelInit(memMb, loglevel);
        if (rc !== 0) throw new Error(`anyfs-native kernelInit failed: rc=${rc}`);
    }

    // ── Lifecycle ─────────────────────────────────────

    async attachFile(_file: File): Promise<void> {
        throw new Error('NativeSession: attachFile not available. Use WasmSession for browser File API support.');
    }

    async attachUrl(url: string, _name?: string): Promise<void> {
        if (this.handle >= 0) throw new Error('attachUrl: already attached');
        const { proxyUrl, id } = await this.bridge.registerUrl(url);
        this.proxyId = id;
        try {
            const h = await this.chain(() => this.bridge.sessionOpen(proxyUrl, 1));
            if (h < 0) throw new Error(`sessionOpen(${proxyUrl}) failed: rc=${h}`);
            this.handle = h;
        } catch (err) {
            await this.bridge.unregisterUrl(id);
            this.proxyId = null;
            throw err;
        }
    }

    async attachPath(path: string): Promise<void> {
        if (this.handle >= 0) throw new Error('attachPath: already attached');
        const h = await this.chain(() => this.bridge.sessionOpen(path, 1));
        if (h < 0) throw new Error(`sessionOpen failed: rc=${h}`);
        this.handle = h;
    }

    // ── Partition / mount ─────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        return this.chain(() => this.bridge.sessionEnter(this.handle, part, flags));
    }

    async listParts(): Promise<SessionPartInfo[]> {
        return this.chain(async () =>
            JSON.parse(await this.bridge.sessionListJson(this.handle)),
        ) as Promise<SessionPartInfo[]>;
    }

    async meta(): Promise<SessionMeta> {
        return this.chain(async () =>
            JSON.parse(await this.bridge.sessionMetaJson(this.handle)),
        ) as Promise<SessionMeta>;
    }

    // ── Filesystem ops ────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        return this.chain(async () => JSON.parse(await this.bridge.readdirJson(path)));
    }

    async stat(path: string): Promise<Stat> {
        return this.chain(async () => JSON.parse(await this.bridge.lstatJson(path)));
    }

    async statFollow(path: string): Promise<Stat> {
        return this.chain(async () => JSON.parse(await this.bridge.statJson(path)));
    }

    async readlink(path: string): Promise<string> {
        return this.chain(() => this.bridge.readlink(path));
    }

    async realpath(path: string): Promise<string> {
        return this.chain(() => this.bridge.realpath(path));
    }

    async readKernelFile(path: string, maxBytes = 64 * 1024): Promise<string> {
        const fd = await this.openFd(path);
        try {
            const chunks: Uint8Array[] = [];
            let offset = 0;
            for (let i = 0; i < 64 && offset < maxBytes; i++) {
                const want = Math.min(8192, maxBytes - offset);
                const chunk = await this.readFd(fd, offset, want);
                if (chunk.length === 0) break;
                chunks.push(chunk);
                offset += chunk.length;
            }
            let total = 0;
            for (const c of chunks) total += c.length;
            const buf = new Uint8Array(total);
            let p = 0;
            for (const c of chunks) { buf.set(c, p); p += c.length; }
            return new TextDecoder('utf-8').decode(buf);
        } finally {
            try { await this.closeFd(fd); } catch { /* best effort */ }
        }
    }

    // ── Events ────────────────────────────────────────

    onProgress(_cb: (step: string) => void): () => void {
        return () => {};
    }

    // ── Internal fd ops ───────────────────────────────

    protected async _openFdRaw(path: string): Promise<LklFd> {
        const fd = await this.chain(() => this.bridge.fileOpen(path, 0));
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        return fd;
    }

    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        const { rc, data } = await this.chain(() => this.bridge.pread(fd, length, offset));
        if (rc < 0) throw new Error(`pread rc=${rc}`);
        return data;
    }

    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        const rc = await this.chain(() => this.bridge.fileClose(fd));
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    protected async _dispose(): Promise<void> {
        try { await this.opChain; } catch { /* best effort */ }
        if (this.handle >= 0) {
            try { await this.bridge.sessionClose(this.handle); } catch { /* best effort */ }
            this.handle = -1;
        }
        if (this.proxyId) {
            try { await this.bridge.unregisterUrl(this.proxyId); } catch { /* best effort */ }
            this.proxyId = null;
        }
    }
}
```

- [ ] **Step 2: Delete old `native-client.ts`**

```bash
git rm ts/packages/core/src/native-client.ts
```

- [ ] **Step 3: Commit**

```bash
git add ts/packages/core/src/native-session.ts && git rm ts/packages/core/src/native-client.ts
git commit -m "refactor(core): rename NativeAnyfsDisk → NativeSession, extend AnyfsSessionBase

Drop openReadable/walk (now in base). Keep chain() serialization.
Expose attachFile/attachUrl/attachPath separately.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: Update worker.ts ccall strings

**Files:**
- Modify: `ts/packages/core/src/worker.ts`

- [ ] **Step 1: Update ccall strings in worker.ts**

The ops table in worker.ts needs renaming to match new conventions. Key changes:

- `boot` → use `anyfs_ts_kernel_init` instead of `anyfs_ts_init`
- `attach` → renamed to `attachFile`
- All `anyfs_ts_disk_open_p` → `anyfs_ts_session_open_p`
- All `anyfs_ts_disk_list_json_p` → `anyfs_ts_session_list_json_p`
- All `anyfs_ts_disk_meta_json_p` → `anyfs_ts_session_meta_json_p`
- `mountWhole` → deleted, merged into `enter` (part=0 path)
- `anyfs_ts_disk_enter_p` → `anyfs_ts_session_enter_p`
- `open` → `openFd`
- `read` → `readFd`
- `close` → `closeFd`

Also rename the op dispatch keys: `listPartitions` → `listParts`, `diskMeta` → `meta`.

- [ ] **Step 2: Update worker.ts ops table**

The ops object keys change:

```typescript
const ops: Record<string, (a: any) => unknown> = {
    async boot(a: BootArgs) {
        // ... same logic but:
        // anyfs_ts_init → anyfs_ts_kernel_init
        // anyfs_ts_init_async → (rename check — keep name or update too)
        // anyfs_ts_is_boot_complete → keep
        // anyfs_ts_boot_result → keep
    },

    async attachFile(a: { file: File }) {
        // ... same logic but:
        // anyfs_ts_disk_open_p → anyfs_ts_session_open_p
    },

    async attachUrl(a: { url: string; name: string }) {
        // ... same logic but:
        // anyfs_ts_disk_open_p → anyfs_ts_session_open_p
    },

    // delete `mount` (back-compat boot+attach — no longer needed)

    listParts() {
        return callJsonOut('anyfs_ts_session_list_json_p', ['number'], [diskHandle]);
    },

    meta() {
        return callJsonOut('anyfs_ts_session_meta_json_p', ['number'], [diskHandle]);
    },

    async enter({ part, flags }: { part: number; flags?: number }) {
        if (!M) throw new Error('not mounted');
        const cap = 128;
        const out = M._malloc(cap);
        try {
            const rc = await callP(
                'anyfs_ts_session_enter_p',
                ['number', 'number', 'number', 'number', 'number'],
                [diskHandle, part, flags ?? 0, out, cap],
            );
            if (rc < 0) throw new Error(`session_enter rc=${rc}`);
            return M.UTF8ToString(out);
        } finally {
            M._free(out);
        }
    },

    // delete mountWhole

    readdir({ path }: { path: string }) {
        return callJsonOutStr('anyfs_ts_readdir_json_p', path);
    },

    stat({ path }: { path: string }) {
        return callJsonOutStr('anyfs_ts_lstat_json_p', path);
    },

    statFollow({ path }: { path: string }) {
        return callJsonOutStr('anyfs_ts_stat_json_p', path);
    },

    readlink({ path }: { path: string }) {
        // same but: anyfs_ts_readlink_p
    },

    realpath({ path }: { path: string }) {
        // same but: anyfs_ts_realpath_p
    },

    readKernelFile({ path }: { path: string }) {
        // same but: anyfs_ts_read_kernel_file_p
    },

    openFd({ path }: { path: string }) {
        return callP('anyfs_ts_open_p', ['string', 'number'], [path, 0]);
    },

    async readFd({ fd, offset, length }: { fd: number; offset: number; length: number }) {
        // same logic but: anyfs_ts_pread_p
    },

    closeFd({ fd }: { fd: number }) {
        return callP('anyfs_ts_close_p', ['number'], [fd]);
    },

    async dispose() {
        // same but: anyfs_ts_session_close instead of anyfs_ts_disk_close
        // anyfs_ts_kernel_halt
    },
};
```

- [ ] **Step 3: Also update async boot path ccall names in the boot handler**

- [ ] **Step 4: Commit**

```bash
git add ts/packages/core/src/worker.ts
git commit -m "refactor(core): update worker.ts ccall strings to anyfs_ts_session_* names

Rename ops: listPartitions→listParts, diskMeta→meta, open/read/close→openFd/readFd/closeFd.
Delete mountWhole op (merged into enter). Drop back-compat 'mount' op.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: Update core index.ts, boot.ts, node.ts exports

**Files:**
- Modify: `ts/packages/core/src/index.ts`
- Modify: `ts/packages/core/src/boot.ts`
- Modify: `ts/packages/core/src/node.ts`
- Modify: `ts/packages/core/tsup.config.ts`

- [ ] **Step 1: Rewrite `index.ts`**

```typescript
/**
 * @anyfs/core — wasm-LKL anyfs binding for browsers + Node.
 */
import type { SessionOpts } from './types.js';
import { WasmSession } from './wasm-session.js';
import { NativeSession, getAnyfsNative } from './native-session.js';
import { getUrlProxyPrefix } from './electron-proxy.js';

// Re-export types
export type { AnyfsSession } from './session.js';
export type { AnyfsNativeBridge } from './native-session.js';
export { WasmSession, NativeSession, getAnyfsNative };
export { applyUrlProxy, getUrlProxyPrefix } from './electron-proxy.js';
export {
    fmtBytes,
    fmtMode,
    fmtTime,
    fmtDev,
    formatSize,
    splitExt,
} from './format.js';
export type {
    SessionHandle,
    LklFd,
    SessionPartInfo,
    DirEntry,
    EntryKind,
    Stat,
    SessionOpts,
    SessionMeta,
    SessionSource,
} from './types.js';

export interface BrowserMountOpts extends SessionOpts {
    workerUrl: string | URL;
    wasmBaseUrl?: string;
    wasmModuleName?: string;
}

export async function prewarm(opts: BrowserMountOpts): Promise<WasmSession> {
    console.log('[PREWARM] creating worker, url=', String(opts.workerUrl));
    const worker = new Worker(opts.workerUrl, { type: 'module' });
    try {
        console.log('[PREWARM] waiting for host-ready...');
        await WasmSession.waitForReady(worker);
        console.log('[PREWARM] host-ready received');
    } catch (err) {
        worker.terminate();
        throw err;
    }
    const session = new WasmSession(worker);
    try {
        console.log('[PREWARM] calling boot...');
        await session.callRaw('boot', {
            memMb: opts.memMb ?? 64,
            loglevel: opts.loglevel ?? 0,
            wasmBaseUrl: opts.wasmBaseUrl ?? '/wasm/',
            wasmModuleName: opts.wasmModuleName ?? 'anyfs.mjs',
            urlProxyPrefix: getUrlProxyPrefix(),
        });
        console.log('[PREWARM] boot complete');
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}

export async function mountFile(file: File, opts: BrowserMountOpts): Promise<WasmSession> {
    const session = await prewarm(opts);
    try {
        await session.attachFile(file);
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}

export async function prewarmNative(
    opts: Pick<SessionOpts, 'memMb' | 'loglevel'> = {},
): Promise<NativeSession | null> {
    const bridge = getAnyfsNative();
    if (!bridge) return null;
    const ok = await bridge.available();
    if (!ok) return null;
    const session = new NativeSession(bridge);
    try {
        await session.boot(opts.memMb ?? 256, opts.loglevel ?? 0);
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}
```

- [ ] **Step 2: Update `boot.ts` to use `NodeWasmSession` and new names**

```typescript
import { NodeWasmSession } from './node-wasm-session.js';
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';

let g_modulePromise: Promise<AnyfsModule> | null = null;
let g_kernelInitialised = false;

export async function bootModule(args: {
    factory: AnyfsModuleFactory;
    preRun: Array<(m: AnyfsModule) => void>;
    memMb: number;
    loglevel: number;
}): Promise<AnyfsModule> {
    if (g_modulePromise) return g_modulePromise;
    g_modulePromise = (async () => {
        const M = await args.factory({ preRun: args.preRun });
        if (!g_kernelInitialised) {
            const rc = M.ccall(
                'anyfs_ts_kernel_init',
                'number',
                ['number', 'number'],
                [args.memMb, args.loglevel],
            ) as number;
            if (rc !== 0) throw new Error(`anyfs_ts_kernel_init failed: ${rc}`);
            g_kernelInitialised = true;
        }
        return M;
    })();
    return g_modulePromise;
}

export async function openSession(M: AnyfsModule, fsPath: string): Promise<NodeWasmSession> {
    const session = new NodeWasmSession(M);
    await session.attachPath(fsPath);
    return session;
}

export async function haltKernel(): Promise<void> {
    if (!g_modulePromise) return;
    const M = await g_modulePromise;
    M.ccall('anyfs_ts_kernel_halt', 'number', [], []);
    g_modulePromise = null;
    g_kernelInitialised = false;
}
```

- [ ] **Step 3: Update `node.ts`**

```typescript
/** Node-only entry — uses NODEFS. */
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import type { SessionOpts } from './types.js';
import { bootModule, openSession, haltKernel as halt } from './boot.js';
import type { NodeWasmSession } from './node-wasm-session.js';

export async function mountNodeFile(
    hostPath: string,
    factory: AnyfsModuleFactory,
    opts: SessionOpts = {},
): Promise<NodeWasmSession> {
    const memMb = opts.memMb ?? 64;
    const loglevel = opts.loglevel ?? 0;
    const { default: path } = await import('node:path');
    const dir = path.dirname(hostPath);
    const base = path.basename(hostPath);
    const M = await bootModule({
        factory,
        memMb,
        loglevel,
        preRun: [
            (m: AnyfsModule) => {
                if (!m.NODEFS) throw new Error('NODEFS not exported');
                m.FS.mkdir('/work');
                m.FS.mount(m.NODEFS, { root: dir }, '/work');
            },
        ],
    });
    return openSession(M, `/work/${base}`);
}

export const haltKernel = halt;
```

- [ ] **Step 4: Update `tsup.config.ts` entry points**

```typescript
import { defineConfig } from 'tsup';

export default defineConfig([
    {
        entry: ['src/index.ts', 'src/node.ts'],
        format: 'esm',
        dts: true,
        clean: true,
        sourcemap: true,
    },
    {
        entry: { 'anyfs.worker': 'src/worker.ts' },
        format: 'esm',
        outDir: 'wasm',
        clean: false,
        sourcemap: true,
        outExtension: () => ({ js: '.js' }),
    },
]);
```

- [ ] **Step 5: Commit**

```bash
git add ts/packages/core/src/index.ts ts/packages/core/src/boot.ts ts/packages/core/src/node.ts ts/packages/core/tsup.config.ts
git commit -m "refactor(core): update exports — WasmSession, NativeSession, NodeWasmSession

Update boot.ts to use NodeWasmSession + new ccall names.
Node entry uses mountNodeFile returning NodeWasmSession.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: Update @anyfs/react provider + hooks

**Files:**
- Modify: `ts/packages/react/src/provider.tsx`
- Modify: `ts/packages/react/src/use-dir.ts`
- Modify: `ts/packages/react/src/use-file.ts`
- Modify: `ts/packages/react/src/index.ts`

- [ ] **Step 1: Update `provider.tsx`**

Key changes:
- Import from new names: `WasmSession`, `NativeSession`, `getAnyfsNative`
- Type references: `AnyfsDisk` → `AnyfsSession`, `DiskSource` → `SessionSource`, `MountOpts` → `SessionOpts`
- `disk.attach(file)` → `session.attachFile(file)`
- `disk.attachUrl(url, name)` → `session.attachUrl(url, name)`
- `disk.attachPath(path)` → `session.attachPath(path)`
- `disk.listPartitions()` → `session.listParts()`
- `disk.diskMeta()` → `session.meta()`
- `disk.mountWhole()` → `session.enter(0)`
- `disk.dispose()` → `session.close()`
- State type property `disk` → stays `session`? Or keep `disk` for compat with existing component props...

Actually, the state type `AnyfsState` has `disk: AnyDisk | null`. We should rename this to `session`. But many components reference `anyfs.disk`. Let me keep the rename to `session` throughout for consistency.

- [ ] **Step 2: Update `use-dir.ts`**

```typescript
import { useEffect, useState } from 'react';
import type { DirEntry } from '@anyfs/core';
import { useAnyfsSession } from './provider.js';

interface DirState {
    entries: DirEntry[] | null;
    loading: boolean;
    error: Error | null;
}

const cache = new WeakMap<object, Map<string, DirEntry[]>>();

export function useAnyfsDir(path: string | null): DirState {
    const { session, status } = useAnyfsSession();
    const [state, setState] = useState<DirState>({
        entries: null, loading: false, error: null,
    });

    useEffect(() => {
        if (!session || status !== 'ready' || !path) return;
        const m = cache.get(session) ?? new Map<string, DirEntry[]>();
        cache.set(session, m);
        const hit = m.get(path);
        if (hit) {
            setState({ entries: hit, loading: false, error: null });
            return;
        }
        let cancelled = false;
        setState({ entries: null, loading: true, error: null });
        session.readdir(path).then(
            (entries) => {
                if (cancelled) return;
                m.set(path, entries);
                setState({ entries, loading: false, error: null });
            },
            (err) => {
                if (cancelled) return;
                setState({ entries: null, loading: false,
                    error: err instanceof Error ? err : new Error(String(err)) });
            },
        );
        return () => { cancelled = true; };
    }, [session, status, path]);

    return state;
}
```

- [ ] **Step 3: Update `use-file.ts`**

```typescript
import { useEffect, useState } from 'react';
import { useAnyfsSession } from './provider.js';

export interface FileRange { offset: number; length: number; }
interface FileState {
    data: Uint8Array | null; size: number | null;
    loading: boolean; error: Error | null;
}

export function useAnyfsFile(path: string | null, range?: FileRange): FileState {
    const { session, status } = useAnyfsSession();
    const [state, setState] = useState<FileState>({
        data: null, size: null, loading: false, error: null,
    });
    const off = range?.offset ?? 0;
    const len = range?.length ?? null;

    useEffect(() => {
        if (!session || status !== 'ready' || !path) return;
        let cancelled = false;
        setState({ data: null, size: null, loading: true, error: null });
        (async () => {
            try {
                const st = await session.stat(path);
                const readLen = len ?? Math.max(0, st.size - off);
                if (readLen === 0) {
                    if (!cancelled) setState({
                        data: new Uint8Array(0), size: st.size,
                        loading: false, error: null,
                    });
                    return;
                }
                const fd = await session.openFd(path);
                try {
                    const data = await session.readFd(fd, off, readLen);
                    if (!cancelled) setState({ data, size: st.size, loading: false, error: null });
                } finally {
                    await session.closeFd(fd).catch(() => {});
                }
            } catch (err) {
                if (cancelled) return;
                setState({ data: null, size: null, loading: false,
                    error: err instanceof Error ? err : new Error(String(err)) });
            }
        })();
        return () => { cancelled = true; };
    }, [session, status, path, off, len]);

    return state;
}
```

- [ ] **Step 4: Update `index.ts` exports**

```typescript
export { AnyfsProvider, useAnyfsSession, useAnyfsSessionMaybe } from './provider';
export { useAnyfsDir } from './use-dir';
export { useAnyfsFile } from './use-file';
export type { FileRange } from './use-file';
export type { AnyfsSessionStatus, AnyfsProviderProps, AnyfsState, AnyfsBackendMode } from './provider';
```

- [ ] **Step 5: Commit**

```bash
git add ts/packages/react/src/
git commit -m "refactor(react): rename useAnyfsDisk → useAnyfsSession, update to new API

Use session.openFd/readFd/closeFd. Import types from @anyfs/core new names.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: Update @anyfs/trees

**Files:**
- Modify: `ts/packages/trees/src/AnyfsFileBrowser.tsx`
- Modify: `ts/packages/trees/src/index.ts`

- [ ] **Step 1: Update imports in AnyfsFileBrowser.tsx**

Key changes:
- Import `AnyfsSession` instead of `AnyfsDisk`
- Import `splitExt`, `fmtBytes` from `@anyfs/core` instead of local helpers
- Remove local `splitExt` and `fmtBytes` implementations
- `useAnyfsDiskMaybe` → `useAnyfsSessionMaybe`
- `disk.readdir()` → `session.readdir()`
- `disk.stat()` → `session.stat()`
- `disk.statFollow()` → `session.statFollow()`
- `disk.realpath()` → `session.realpath()`
- `disk.readlink()` → `session.readlink()`
- Props type: `disk?: AnyfsDisk` → `session?: AnyfsSession`

- [ ] **Step 2: Update `index.ts`**

```typescript
export { AnyfsFileBrowser } from './AnyfsFileBrowser';
export type { AnyfsFileBrowserProps } from './AnyfsFileBrowser';

// Back-compat alias
export { AnyfsFileBrowser as FileTreeView } from './AnyfsFileBrowser';
export type { AnyfsFileBrowserProps as FileTreeViewProps } from './AnyfsFileBrowser';
```

- [ ] **Step 3: Commit**

```bash
git add ts/packages/trees/src/
git commit -m "refactor(trees): update to AnyfsSession, import fmtBytes/splitExt from @anyfs/core

Drop duplicate splitExt/fmtBytes helpers. Use session.* method names.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 12: Split App.tsx into components

**Files:**
- Create: `ts/examples/vite-demo/src/TopBar.tsx`
- Create: `ts/examples/vite-demo/src/FilePicker.tsx`
- Create: `ts/examples/vite-demo/src/DiskView.tsx`
- Create: `ts/examples/vite-demo/src/Dialogs.tsx`
- Create: `ts/examples/vite-demo/src/KernelStatusBar.tsx`
- Create: `ts/examples/vite-demo/src/SupportedFormats.tsx`
- Create: `ts/examples/vite-demo/src/DownloadStatus.tsx`
- Create: `ts/examples/vite-demo/src/AboutDialog.tsx`
- Modify: `ts/examples/vite-demo/src/App.tsx` → thin shell

- [ ] **Step 1: Extract `TopBar.tsx`**

Move the `TopBar` component from App.tsx into its own file. Accepts props: `onOpenSettings`, `onOpenAbout`, `onHomeClick?`, `imageName?`, `onImageClick?`, `partitionLabel?`.

- [ ] **Step 2: Extract `KernelStatusBar.tsx`**

Move `KernelStatusBar` component. Uses `useAnyfsSessionMaybe()` from `@anyfs/react`.

- [ ] **Step 3: Extract `SupportedFormats.tsx`**

Move `SupportedFormats`, `FS_GROUPS`, `FALLBACK_FS`, `IMAGE_FORMATS`, `parseProcFilesystems`. Uses `useAnyfsSession()`.

- [ ] **Step 4: Extract `AboutDialog.tsx`**

Move `AboutDialog` component. Pure presentational — no context dependency.

- [ ] **Step 5: Extract `Dialogs.tsx`**

Move: `ConfirmDialog`, `UrlPromptDialog`, `UrlErrorDialog`, `SystemDrivesDialog`. Also `probeUrlAhead`, `SysDrive`, `SysPartition`, `SysMountpoint` types, `getElectronDrives()`, `getElectronDialog()`.

- [ ] **Step 6: Extract `FilePicker.tsx`**

Move: `FilePicker`, `RecentsList`, `formatSize` (now imported from `@anyfs/core`), `formatTs`, `sourceName`.

- [ ] **Step 7: Extract `DiskView.tsx`**

Move: `DiskView`, `DownloadingFileTree`, `DiskSummary`, `DownloadJob`, `DownloadStatus`, `ptLabel`, `fmtBytes` (now imported from `@anyfs/core`).

Wait — `DownloadStatus` should be its own file. Let me adjust:

- **Extract `DownloadStatus.tsx`**: `DownloadStatus`, `DownloadJob` interface.

- [ ] **Step 8: Rewrite `App.tsx` as thin shell**

```typescript
import { useCallback, useEffect, useState } from 'react';
import { AnyfsProvider } from '@anyfs/react';
import type { SessionSource } from '@anyfs/core';
import { getAnyfsNative, getUrlProxyPrefix } from '@anyfs/core';
import { SettingsProvider } from './Settings';
import { TopBar } from './TopBar';
import { FilePicker } from './FilePicker';
import { DiskView } from './DiskView';
import { KernelStatusBar } from './KernelStatusBar';
import { SettingsDialog } from './Settings';
import { AboutDialog } from './AboutDialog';
import { ConfirmDialog } from './Dialogs';

const WORKER_URL = new URL('/wasm/anyfs.worker.js', window.location.href).href;

function clearNavHash() {
    if (typeof window === 'undefined') return;
    if (!window.location.hash) return;
    window.history.replaceState(null, '', window.location.pathname + window.location.search);
}

export function App() {
    const [source, setSource] = useState<SessionSource | null>(null);
    const [selectedPart, setSelectedPart] = useState<number | null>(null);
    const [settingsOpen, setSettingsOpen] = useState(false);
    const [aboutOpen, setAboutOpen] = useState(false);
    const [confirm, setConfirm] = useState<ConfirmCfg | null>(null);

    // CDP test hook
    useEffect(() => {
        (window as any).__anyfsTest = {
            openUrl: (url: string) => setSource({ kind: 'url', url, name: url.split('/').pop() || url }),
            openPath: (path: string) => setSource({ kind: 'path', path }),
            setSourceFile: (file: File) => setSource({ kind: 'file', file }),
        };
        return () => { delete (window as any).__anyfsTest; };
    }, []);

    const settingsDisableNative = (() => {
        try {
            if (typeof localStorage !== 'undefined') {
                const raw = localStorage.getItem('anyfs.settings.v1');
                if (raw) return !!(JSON.parse(raw) as Record<string, unknown>).disableNative;
            }
        } catch {}
        return false;
    })();

    return (
        <SettingsProvider>
            <AnyfsProvider
                source={source}
                workerUrl={WORKER_URL}
                wasmBaseUrl="/wasm/"
                wasmModuleName="anyfs.qemu.mjs"
                autoMountFstype="auto"
                mountOpts={{ loglevel: 7 }}
                prewarm
                {...(settingsDisableNative ? { forceMode: 'wasm' as const } : {})}
            >
                <div className="h-screen flex flex-col">
                    <TopBar
                        onOpenSettings={() => setSettingsOpen(true)}
                        onOpenAbout={() => setAboutOpen(true)}
                    />
                    <main className="flex-1 min-h-0 flex flex-col overflow-y-auto">
                        {source ? (
                            <DiskView
                                source={source}
                                selectedPart={selectedPart}
                                setSelectedPart={setSelectedPart}
                            />
                        ) : (
                            <FilePicker onSource={setSource} />
                        )}
                    </main>
                    <KernelStatusBar />
                    <SettingsDialog open={settingsOpen} onClose={() => setSettingsOpen(false)}
                        nativeAvailable={!!getAnyfsNative()} />
                    <AboutDialog open={aboutOpen} onClose={() => setAboutOpen(false)} />
                    {confirm && (
                        <ConfirmDialog {...confirm} onCancel={() => setConfirm(null)} />
                    )}
                </div>
            </AnyfsProvider>
        </SettingsProvider>
    );
}
```

- [ ] **Step 9: Commit**

```bash
git add ts/examples/vite-demo/src/
git commit -m "refactor(vite-demo): split App.tsx into focused components

Extract: TopBar, FilePicker, DiskView, Dialogs, KernelStatusBar,
SupportedFormats, DownloadStatus, AboutDialog.
App.tsx is now a thin shell (~80 lines).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 13: Update tests

**Files:**
- Modify: `ts/packages/core/test/smoke.node.mjs`
- Modify: `ts/packages/core/test/api.node.mjs`
- Modify: `ts/packages/core/test/openReadable.node.mjs`
- Modify: `ts/packages/core/test/smoke.native.mjs`
- Create: `ts/packages/core/test/debian-iso.native.mjs`
- Modify: `ts/packages/anyfs-native/test/smoke.mjs`
- Modify: `ts/packages/anyfs-native/test/smoke-url.mjs`
- Delete: `test_debian_crash.mjs`, `test_http.mjs`, `test_targeted.mjs`

- [ ] **Step 1: Update `smoke.native.mjs` bridge adapter to new addon names**

The addon now exports `kernelInit`/`sessionOpen`/`sessionClose`/`sessionListJson`/`sessionMetaJson`/`sessionEnter` (no more `mountWhole`). Update the bridge:

```javascript
import { NativeSession } from '@anyfs/core';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const addon = require('../../anyfs-native/build/Release/anyfs_native.node');
const here = dirname(fileURLToPath(import.meta.url));
const img =
    process.argv[2] ||
    process.env.ANYFS_NATIVE_IMAGE ||
    resolve(here, '../../../examples/vite-demo/public/disks/multi.img');

const bridge = {
    available: async () => true,
    init: async (m, l) => addon.kernelInit(m, l),
    sessionOpen: async (p, f) => addon.sessionOpen(p, f),
    sessionClose: async (h) => addon.sessionClose(h),
    sessionListJson: async (h) => addon.sessionListJson(h),
    sessionMetaJson: async (h) => addon.sessionMetaJson(h),
    sessionEnter: async (h, p, f) => addon.sessionEnter(h, p, f),
    readdirJson: async (p) => addon.readdirJson(p),
    lstatJson: async (p) => addon.lstatJson(p),
    statJson: async (p) => addon.statJson(p),
    realpath: async (p) => addon.realpath(p),
    readlink: async (p) => addon.readlink(p),
    fileOpen: async (p, f) => addon.fileOpen(p, f),
    pread: async (fd, n, off) => {
        const buf = new Uint8Array(n);
        const rc = addon.pread(fd, buf, n, off);
        if (rc < 0) return { rc, data: new Uint8Array(0) };
        return { rc, data: rc === buf.length ? buf : buf.subarray(0, rc) };
    },
    fileClose: async (fd) => addon.fileClose(fd),
};

const session = new NativeSession(bridge);
let failed = false;
try {
    console.error('[1/5] boot kernel');
    await session.boot(256, 0);

    console.error(`[2/5] attachPath(${img})`);
    await session.attachPath(img);

    console.error('[3/5] meta + listParts');
    const meta = await session.meta();
    const parts = await session.listParts();
    console.error(`     pt=${meta.pt_type} parts=${parts.length}`);
    if (parts.length < 1) throw new Error('expected at least one partition');

    console.error('[4/5] enter(part 2) and readdir /');
    const mp = await session.enter(2);
    console.error(`     mount=${mp}`);
    const entries = await session.readdir(mp);
    console.error(`     entries=${entries.map((e) => e.name).join(',')}`);

    console.error('[5/5] close');
    await session.close();
    console.error('OK — NativeSession end-to-end pass');
} catch (e) {
    console.error('FAIL:', e?.stack ?? e);
    failed = true;
} finally {
    process.exit(failed ? 1 : 0);
}
```

- [ ] **Step 2: Update import paths and type/method references in remaining test files**

Key changes across `smoke.node.mjs`, `api.node.mjs`, `openReadable.node.mjs`, `anyfs-native/test/smoke.mjs`, `anyfs-native/test/smoke-url.mjs`:
- `AnyfsDisk` → `AnyfsSession` (interface) or `NodeWasmSession` (constructor)
- `DiskSource` → `SessionSource`
- `MountOpts` → `SessionOpts`
- `mountNodeFile` returns `NodeWasmSession` now
- `disk.mountWhole(fstype)` → `session.enter(0)`
- `disk.listPartitions()` → `session.listParts()`
- `disk.diskMeta()` → `session.meta()`
- `disk.dispose()` → `session.close()`
- `disk.attach(file)` → `session.attachFile(file)`
- `disk.attachUrl(url)` → `session.attachUrl(url)`
- `disk.open(path)` → `session.openFd(path)`
- `disk.read(fd, offset, len)` → `session.readFd(fd, offset, len)`
- `disk.close(fd)` → `session.closeFd(fd)`

For `anyfs-native/test/smoke.mjs` (low-level addon): `addon.init` → `addon.kernelInit`, `addon.diskOpen` → `addon.sessionOpen`, `addon.diskListJson` → `addon.sessionListJson`, `addon.diskMetaJson` → `addon.sessionMetaJson`, `addon.diskClose` → `addon.sessionClose`.

For `anyfs-native/test/smoke-url.mjs` (low-level addon): same addon export renames + `addon.kernelHalt`.

- [ ] **Step 3: Create `debian-iso.native.mjs` — merge 3 root scripts into one proper test**

```javascript
#!/usr/bin/env node
// Debian ISO smoke: iso9660 whole-disk + NESTED partition recursion.
// Uses NativeSession via the addon bridge (same pattern as smoke.native.mjs).
//
//   DEBIAN_ISO=/path/to/debian.iso node debian-iso.native.mjs
//   node debian-iso.native.mjs /path/to/debian.iso
//
// Skips gracefully (exit 0) if no ISO found — no default path.

import { NativeSession } from '@anyfs/core';
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { strict as assert } from 'node:assert';

const iso = process.argv[2] || process.env.DEBIAN_ISO;
if (!iso) {
    console.error('SKIP: set DEBIAN_ISO env var or pass path as arg');
    process.exit(0);
}
if (!existsSync(iso)) {
    console.error(`SKIP: ${iso} not found`);
    process.exit(0);
}

const require = createRequire(import.meta.url);
const addon = require('../../anyfs-native/build/Release/anyfs_native.node');

const bridge = {
    available: async () => true,
    init: async (m, l) => addon.kernelInit(m, l),
    sessionOpen: async (p, f) => addon.sessionOpen(p, f),
    sessionClose: async (h) => addon.sessionClose(h),
    sessionListJson: async (h) => addon.sessionListJson(h),
    sessionMetaJson: async (h) => addon.sessionMetaJson(h),
    sessionEnter: async (h, p, f) => addon.sessionEnter(h, p, f),
    readdirJson: async (p) => addon.readdirJson(p),
    lstatJson: async (p) => addon.lstatJson(p),
    statJson: async (p) => addon.statJson(p),
    realpath: async (p) => addon.realpath(p),
    readlink: async (p) => addon.readlink(p),
    fileOpen: async (p, f) => addon.fileOpen(p, f),
    pread: async (fd, n, off) => {
        const buf = new Uint8Array(n);
        const rc = addon.pread(fd, buf, n, off);
        if (rc < 0) return { rc, data: new Uint8Array(0) };
        return { rc, data: rc === buf.length ? buf : buf.subarray(0, rc) };
    },
    fileClose: async (fd) => addon.fileClose(fd),
};

const session = new NativeSession(bridge);
let failed = false;
try {
    console.error('[1/7] boot kernel (mem=256)');
    await session.boot(256, 0);

    console.error(`[2/7] attachPath(${iso})`);
    await session.attachPath(iso);

    console.error('[3/7] meta');
    const meta = await session.meta();
    console.error(`     logical_size=${meta.logical_size} pt_type=${meta.pt_type || '(none)'}`);

    console.error('[4/7] whole-disk mount: enter(0)');
    const mp = await session.enter(0);
    console.error(`     mount=${mp}`);
    assert.ok(mp && mp.length > 0, 'enter(0) returned empty mount path');

    const root = await session.readdir(mp);
    console.error(`     root entries: ${root.length}`);
    for (const f of root.slice(0, 10)) console.error(`       ${f.name} (${f.kind})`);
    assert.ok(root.length > 0, 'iso9660 root is empty');

    console.error('[5/7] listParts — check for NESTED partitions');
    const parts = await session.listParts();
    console.error(`     ${parts.length} top-level partitions`);
    const nested = parts.filter(p => p.kind === 'NESTED_PARTITION_TABLE' || p.kind === 'LVM_PV' || p.kind === 'LUKS');
    console.error(`     nested/container: ${nested.map(p => `[${p.index}] ${p.kind} fstype=${p.fstype || '?'}`).join(', ') || 'none'}`);

    if (nested.length > 0) {
        console.error(`[6/7] enter NESTED partition [${nested[0].index}]`);
        const nestedMp = await session.enter(nested[0].index);
        console.error(`     mount=${nestedMp || '(empty — container, no FS mount)'}`);

        // list children of the nested container
        const children = await session.listParts();
        console.error(`     children after enter: ${children.length}`);
        const fsChildren = children.filter(p => p.parent >= 0 && p.kind === 'FS');
        console.error(`     FS children: ${fsChildren.map(p => `[${p.index}] ${p.fstype || '?'} label="${p.label}"`).join(', ') || 'none'}`);

        if (fsChildren.length > 0) {
            const childMp = await session.enter(fsChildren[0].index);
            console.error(`     child FS mount=${childMp}`);
            assert.ok(childMp && childMp.length > 0, 'child FS enter returned empty path');
            const childEntries = await session.readdir(childMp);
            console.error(`     child entries: ${childEntries.length}`);
            for (const f of childEntries.slice(0, 5)) console.error(`       ${f.name} (${f.kind})`);
        }
    } else {
        console.error('[6/7] SKIP — no nested/container partitions');
    }

    console.error('[7/7] close');
    await session.close();
    console.error('OK — Debian ISO end-to-end pass');
} catch (e) {
    console.error('FAIL:', e?.stack ?? e);
    failed = true;
} finally {
    process.exit(failed ? 1 : 0);
}
```

- [ ] **Step 4: Delete root-level ad-hoc scripts**

```bash
git rm /home/kosaka/anyfs-reader/test_debian_crash.mjs
git rm /home/kosaka/anyfs-reader/test_http.mjs
git rm /home/kosaka/anyfs-reader/test_targeted.mjs
```

- [ ] **Step 5: Commit**

```bash
git add ts/packages/core/test/ ts/packages/anyfs-native/test/
git add test_debian_crash.mjs test_http.mjs test_targeted.mjs  # staged as deletions
git commit -m "test: update to new session API, merge ad-hoc scripts into debian-iso.native.mjs

Replace test_debian_crash/test_http/test_targeted with a single proper test.
Update bridge adapter in smoke.native.mjs for new addon export names
(kernelInit, sessionOpen, sessionClose, sessionListJson, sessionMetaJson,
sessionEnter).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 14: Build verification + test matrix

Test matrix coverage — combinations and which tests exercise them:

| Platform | Backend | Source | Test | Automated? |
|---|---|---|---|---|
| Node | wasm (NodeWasmSession) | path | `core/test/smoke.node.mjs` | Yes |
| Node | wasm (NodeWasmSession) | path | `core/test/api.node.mjs` | Yes |
| Node | wasm (NodeWasmSession) | path | `core/test/openReadable.node.mjs` | Yes |
| Node | native (NativeSession) | path | `core/test/smoke.native.mjs` | Yes |
| Node | native (NativeSession) | path | `anyfs-native/test/smoke.mjs` | Yes |
| Node | native (NativeSession) | url | `anyfs-native/test/smoke-url.mjs` | Yes |
| Node | native (NativeSession) | path (ISO) | `core/test/debian-iso.native.mjs` | Yes* |
| Web | wasm (WasmSession) | url | vite-demo manual | No |
| Web | wasm (WasmSession) | file | vite-demo manual | No |
| Electron | wasm (WasmSession) | url | electron-demo manual | No |
| Electron | wasm (WasmSession) | file | electron-demo manual | No |
| Electron | native (NativeSession) | path | electron-demo manual | No |
| Electron | native (NativeSession) | url | electron-demo manual | No |
| Node | wasm (NodeWasmSession) | url | N/A (WasmSession handles URL) | — |
| Web | native (NativeSession) | N/A | N/A (browser no addon) | — |

**⚠ Pre-existing bugs:** Running these tests may surface bugs that predate this refactoring (e.g., Electron+Wasm URLFS has been broken since a prior change, Electron+Native http-proxy issues, etc.). If a test fails, first check whether the failure is caused by the refactoring (wrong names, missing methods) or a pre-existing issue. Pre-existing bugs should be documented in the test output or a tracking issue — they are NOT within scope to fix as part of this refactoring. The goal is: the tests should work *no worse than before*. Fix assertions and type errors from the rename. Don't fix bugs in the wasm/native backends themselves.

- [ ] **Step 1: Install dependencies and build all packages**

```bash
cd /home/kosaka/anyfs-reader/ts && pnpm install && pnpm -r build 2>&1
```

Expected: core, react, trees all build successfully, no type errors.

- [ ] **Step 2: Run TypeScript checker on vite-demo**

```bash
cd /home/kosaka/anyfs-reader/ts/examples/vite-demo && npx tsc --noEmit 2>&1
```

Expected: no type errors.

- [ ] **Step 3: Run `smoke.node.mjs` — Node × wasm × path (multi image)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/smoke.node.mjs multi 2>&1
```

Expected: "PASS". Covers: `bootModule` → `openSession` → `session.meta()` → `session.listParts()` → `session.enter(1)` → `session.readdir()` → `session.stat()` → `session.openFd()` / `session.readFd()` / `session.closeFd()` → `session.close()` → `haltKernel`.

- [ ] **Step 4: Run `smoke.node.mjs` — Node × wasm × path (single image)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/smoke.node.mjs single 2>&1
```

Expected: "PASS". Covers: `session.enter(0)` whole-disk mount path.

- [ ] **Step 5: Run `api.node.mjs` — Node × wasm × path (single image)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/api.node.mjs single 2>&1
```

Expected: "PASS". Covers: `mountNodeFile()` public API.

- [ ] **Step 6: Run `api.node.mjs` — Node × wasm × path (multi image)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/api.node.mjs multi 2>&1
```

Expected: "PASS".

- [ ] **Step 7: Run `openReadable.node.mjs` — Node × wasm × path (single image)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/openReadable.node.mjs single 2>&1
```

Expected: "PASS". Covers: `session.openReadable()` streaming.

- [ ] **Step 8: Run `openReadable.node.mjs` — Node × wasm × path (big image)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/openReadable.node.mjs big 2>&1
```

Expected: "PASS". Covers: multi-chunk streaming (>1MB).

- [ ] **Step 9: Run `smoke.native.mjs` — Node × native × path**

```bash
node /home/kosaka/anyfs-reader/ts/packages/core/test/smoke.native.mjs 2>&1
```

Expected: "PASS". Covers: `NativeSession` → `boot()` → `attachPath()` → `listParts()` → `meta()` → `enter()` → `readdir()` → `stat()` → `openFd()` / `readFd()` / `closeFd()` → `close()`.

- [ ] **Step 10: Run `anyfs-native/test/smoke.mjs` — Node × native × path (low-level)**

```bash
node /home/kosaka/anyfs-reader/ts/packages/anyfs-native/test/smoke.mjs 2>&1
```

Expected: "PASS". Covers: low-level addon `init()` / `diskOpen()` / `diskListJson()` / `diskMetaJson()` — validates that C glue renames (`anyfs_ts_kernel_init`, `anyfs_ts_session_*`) work in the N-API addon.

- [ ] **Step 11: Run `smoke-url.mjs` — Node × native × url**

```bash
node /home/kosaka/anyfs-reader/ts/packages/anyfs-native/test/smoke-url.mjs 2>&1
```

Expected: "PASS". Covers: QEMU curl block driver linked correctly, URL→proxy→sessionOpen path.

- [ ] **Step 12: Run `debian-iso.native.mjs` — Node × native × path (Debian ISO) — optional**

```bash
# Only if you have a Debian ISO available
DEBIAN_ISO=/home/kosaka/debian-13.5.0-amd64-netinst.iso \
  node /home/kosaka/anyfs-reader/ts/packages/core/test/debian-iso.native.mjs 2>&1
```

Expected: "PASS" or "SKIP" (if ISO not found). Covers: iso9660 whole-disk mount via `enter(0)`, NESTED partition enumeration, child-partition recursion.

- [ ] **Step 13: Commit any build/test fixes**

```bash
git add -A && git commit -m "chore: build and test fixes after TS refactoring

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```
