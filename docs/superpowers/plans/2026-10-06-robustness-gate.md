# Robustness Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn "no corrupt image hangs or crashes the wasm sandbox" into a checked, local pre-release gate: a product watchdog on every session op, hardened mount options, a ~78-case generated corpus plus ~20 syzbot images, a per-case child-process harness for wasm and native, and three E2E cases.

**Architecture:** `AnyfsSessionBase.guard()` wraps every post-attach engine op in a timeout that rejects and fires `onFatal`; `WasmApi.fail()` turns a wasm `abort()` into the same fatal for `NodeWasmSession`. A pure C helper adds `errors=continue` for filesystems whose superblock can request a panic. Under `ts/tests/robustness/`, `corpus/generate.mjs` builds rootless base images and pure, seeded mutations; `run.mjs` forks one `case-runner.mjs` per case and classifies each as ok / error / fatal / hang / crash.

**Tech Stack:** TypeScript (`@anyfs/core`, `@anyfs/react`, `@anyfs/trees`, tsup), Node 24 ESM `.mjs` + `node:test`, C11 + meson, emscripten wasm bundle, N-API addon, Playwright.

**Spec:** `docs/superpowers/specs/2026-10-05-robustness-gate-design.md`

---

## Ground rules for the executor

- Repo root: `/home/kosaka/anyfs-reader`. All paths below are relative to it.
- Everything written into the repo (code, comments, docs, commit messages) is English.
- Commit straight to `main` after each task, no PRs. Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```
- Do **not** push. Other sessions also commit to local `main`. Pushing happens only in Task 19, after
  checking `git log origin/main..main` and getting the user's consent.
- Scratch and corpus data go under `~/.cache` (`/tmp` is a small 7.9 G tmpfs here).
- Format new or changed TS/JS files with `pnpm -C ts exec prettier --write <files>` before committing.
  Prettier config: 4-space indent, single quotes, trailing commas, printWidth 100.
- C style follows the surrounding file: tabs, kernel style, `/* */` comments.
- `@anyfs/core` unit tests import `../dist/*.js`, so rebuild with `pnpm -C ts -F @anyfs/core build`
  before running them.

## Facts established while planning (don't re-derive)

- Tools found: `mke2fs`, `debugfs`, `mkfs.fat`, `mkfs.f2fs`, `mkntfs`, `mkfs.btrfs` (6.14),
  `mkfs.xfs` (6.13), `sfdisk`, `mksquashfs`, `qemu-img` are installed (most in `/sbin`, which is not on
  the user PATH). `mtools` (mcopy), `xorriso` and `exfatprogs` are **not** installed.
- Small images build rootless: btrfs needs `--mixed` to stay at 32 MiB; xfs below 300 MB needs env
  `TEST_DIR=1 TEST_DEV=1 QA_CHECK_FS=1`; `mkntfs -F` works on a plain file (prints warnings, exits 0).
  `sfdisk` on a file works rootless and numbers a logical partition `5`.
- Checksums, verified against real mkfs output: ext4 superblock `s_checksum` (offset 1020) is the
  **raw** crc32c (seed `~0`, no final inversion) over bytes 0..1019; btrfs superblock csum (offset 0)
  is the **standard** crc32c over `[0x20, 0x1000)`; xfs `sb_crc` (offset 224) is the standard crc32c
  over the first `sb_sectsize` bytes with the crc field zeroed. GPT uses zlib's CRC-32.
- btrfs `--mixed` defaults to DUP metadata: every tree has 2+ copies on disk, so a "zero this tree"
  mutation must zero every block whose header owner matches.
- ext4 honours a superblock `errors=panic` (`mke2fs -e panic` sets it) even on a read-only mount:
  `ext4_handle_error()` panics before checking read-only. `ext4_iget()` on a zeroed inode fails its
  checksum and calls that path. That gives a deterministic generated reproduction of acceptance
  criterion 4.
- `errors=` is accepted by ext2/3/4, fat (vfat/msdos), exfat, f2fs and the OOT NTFS PLUS driver
  (registers as `"ntfs"`, enum `panic|remount-ro|continue`). OOT APFS has no `errors=` option, so it
  is left unchanged.
- emscripten proxies `Module.onAbort` from pthreads to the module-owning thread
  (`knownHandlers = ["onExit","onAbort",...]`). In Node the pthread's error is then rethrown on the
  main thread as an uncaught exception, so the harness mirrors `worker.ts`: an uncaught error or
  rejection there becomes a fatal.
- `NodeWasmSession.attachPath` currently opens with flags `0` (read-write), and ext4 writes its
  superblock back on error. The harness therefore opens read-only (new `readOnly` option, like the
  browser worker's flags `1`), and generated images are `chmod 0444` as a backstop.
- When a source is open, `FilePicker` is not rendered, so the E2E recovery step must
  `driver.close()` before `driver.openImage(good)`.
- A failed `readdir` currently renders an empty folder (`AnyfsFileBrowser` just logs it). The E2E
  read case needs a visible error, which Task 17 adds.
- The tsup build shares one chunk between `dist/index.js` and `dist/node.js`, so `wasmApiFor(M)` is
  a single instance across both entries.

## File map

| File | Status | Responsibility |
|---|---|---|
| `ts/packages/core/src/session-base.ts` | modify | `DEFAULT_OP_TIMEOUT_MS`, `SessionBaseOpts`, `EngineFatalError`, `guard()`, `fatalError`, fatal-safe `close()` |
| `ts/packages/core/src/wasm-session.ts` | modify | guarded ops (`op()`), constructor opts |
| `ts/packages/core/src/native-session.ts` | modify | guarded ops inside the op chain, drop `engineFailed` |
| `ts/packages/core/src/wasm-api.ts` | modify | `fail()` / `onFail()` / `failed`, rejectable pending calls |
| `ts/packages/core/src/node-wasm-session.ts` | modify | guarded ops, abort → fatal, `readOnly` |
| `ts/packages/core/src/boot.ts` | modify | `onAbort` hook → `WasmApi.fail`, `openNodeSession` opts |
| `ts/packages/core/src/node.ts` | modify | `bootNodeKernel`, `NodeMountOpts`, re-export `openNodeSession` |
| `ts/packages/core/src/module.ts` | modify | factory `onAbort` option |
| `ts/packages/core/src/types.ts` | modify | `SessionOpts.opTimeoutMs` |
| `ts/packages/core/src/index.ts` | modify | exports; pass `opTimeoutMs` in `prewarm` / `prewarmNative` |
| `ts/packages/core/test/watchdog.test.mjs` | create | base guard unit tests |
| `ts/packages/core/test/wasm-session.test.mjs` | create | WasmSession watchdog tests |
| `ts/packages/core/test/native-watchdog.test.mjs` | create | NativeSession watchdog tests |
| `ts/packages/core/test/node-wasm-session.test.mjs` | create | NodeWasmSession watchdog + abort tests |
| `ts/packages/react/src/provider.tsx` | modify | forward `mountOpts.opTimeoutMs` |
| `ts/examples/vite-demo/src/components/DiskView.tsx` | modify | native engine-fatal hint |
| `src/core/anyfs_mount_opts.{c,h}` | create | pure per-fs mount option builder |
| `src/core/anyfs_mount.c` | modify | use `anyfs_mount_opts()` in both mount paths |
| `tests/unit/test_mount_opts.c` | create | C unit test |
| `meson.build`, `scripts/build_anyfs_wasm.sh` | modify | add the new source / test |
| `ts/tests/robustness/lib/paths.mjs` | create | cache dir + artifact paths |
| `ts/tests/robustness/corpus/{rng,checksum,tree,tools,bases,layout,mutations,generate}.mjs` | create | corpus |
| `ts/tests/robustness/syzbot.json`, `fetch-syzbot.mjs`, `tools/list-syzbot-candidates.mjs` | create | syzbot set |
| `ts/tests/robustness/lib/{glob,classify,walk,report,native-bridge}.mjs` | create | harness pieces |
| `ts/tests/robustness/case-runner.mjs`, `run.mjs` | create | harness |
| `ts/tests/robustness/test/{corpus,harness}.test.mjs` | create | node:test suites |
| `ts/tests/robustness/README.md`, `FINDINGS.md` | create | docs, findings |
| `ts/packages/trees/src/AnyfsFileBrowser.tsx` | modify | visible readdir error (`dir-error`) |
| `ts/tests/e2e/drivers/{driver,dom-actions,web-driver,electron-driver}.ts` | modify | `status()`, `read-failed` |
| `ts/tests/e2e/fixtures/robustness.ts`, `flows/robustness.spec.ts` | create | E2E |
| `ts/tests/e2e/playwright.config.ts` | modify | keep robustness spec off electron-native |

---

### Task 0: Prerequisites and baseline

**Files:** none

- [ ] **Step 1: Ask the user to approve the missing packages**

The corpus needs `mcopy` (vfat population), `xorriso` (iso9660) and `mkfs.exfat` (exfat base). Ask the
user (in Chinese) for approval to run:

```bash
sudo apt-get install -y mtools xorriso exfatprogs
```

If they decline `exfatprogs`, delete the `exfat` entry from `BASES` in Task 7 and change the expected
base count from 16 to 15 (and total 78 → 77) in the Task 7/8 tests. `mtools` and `xorriso` are
required.

- [ ] **Step 2: Verify the toolchain**

Run:
```bash
for t in mke2fs debugfs mkfs.fat mcopy mkfs.exfat mkfs.f2fs mkntfs mkfs.btrfs mkfs.xfs xorriso mksquashfs qemu-img sfdisk; do
  printf '%-12s %s\n' $t "$(PATH=/usr/sbin:/sbin:$PATH command -v $t || echo MISSING)"; done
```
Expected: no `MISSING` (except `mkfs.exfat` if the user declined it).

- [ ] **Step 3: Verify the build artifacts exist and the unit tests pass**

Run:
```bash
ls -la ts/packages/core/wasm/anyfs.node.mjs ts/packages/core/wasm/anyfs.mjs \
       ts/packages/anyfs-native/build/Release/anyfs_native.node build-anyfs-linux-amd64/libanyfs_core.a
pnpm -C ts -F @anyfs/core build && pnpm -C ts -F @anyfs/core test:unit
```
Expected: all four files exist; the core unit tests report `# fail 0`.

---

### Task 1: Base watchdog in `AnyfsSessionBase`

**Files:**
- Modify: `ts/packages/core/src/session-base.ts`
- Modify: `ts/packages/core/src/index.ts:20` (exports)
- Test: `ts/packages/core/test/watchdog.test.mjs`

- [ ] **Step 1: Write the failing test**

Create `ts/packages/core/test/watchdog.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AnyfsSessionBase, DEFAULT_OP_TIMEOUT_MS } from '../dist/index.js';

const never = () => new Promise(() => {});

/** A session whose ops all go through the base watchdog. `impl` maps an op
 *  name to what the transport does; unlisted ops never answer. */
class GuardedSession extends AnyfsSessionBase {
    constructor(opts, impl = {}) {
        super(opts);
        this.impl = impl;
        this.calls = [];
        this.closedFds = [];
    }
    run(op, ...args) {
        return this.guard(op, () => {
            this.calls.push(op);
            return (this.impl[op] ?? never)(...args);
        });
    }
    async attachBlob() {}
    async attachUrl() {}
    async attachPath() {}
    enter(part) {
        return this.run('enter', part);
    }
    listParts() {
        return this.run('listParts');
    }
    meta() {
        return this.run('meta');
    }
    readdir(p) {
        return this.run('readdir', p);
    }
    stat(p) {
        return this.run('stat', p);
    }
    statFollow(p) {
        return this.run('statFollow', p);
    }
    readlink(p) {
        return this.run('readlink', p);
    }
    realpath(p) {
        return this.run('realpath', p);
    }
    readKernelFile(p) {
        return this.run('readKernelFile', p);
    }
    onProgress() {
        return () => {};
    }
    _openFdRaw(p) {
        return this.run('open', p);
    }
    _readFdRaw(fd, off, n) {
        return this.run('read', fd, off, n);
    }
    _closeFdRaw(fd) {
        this.closedFds.push(fd);
        return this.run('close', fd);
    }
    async _dispose() {
        this.disposedCalled = true;
    }
}

test('the default op watchdog is 60 s', () => {
    assert.equal(DEFAULT_OP_TIMEOUT_MS, 60_000);
    assert.equal(new GuardedSession().opTimeoutMs, 60_000);
});

test('a wedged op rejects and fires onFatal within opTimeoutMs', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 50 });
    const fatals = [];
    s.onFatal((e) => fatals.push(e));
    const t0 = Date.now();
    await assert.rejects(s.readdir('/x'), /readdir timed out after 0\.05s — the engine is wedged/);
    const dt = Date.now() - t0;
    assert.ok(dt >= 45 && dt < 1000, `took ${dt} ms`);
    assert.equal(fatals.length, 1);
    assert.equal(fatals[0].name, 'EngineFatalError');
    assert.match(fatals[0].message, /readdir timed out/);
});

test('after a fatal, ops reject at once without reaching the transport', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 20 });
    await assert.rejects(s.readdir('/x'));
    const before = s.calls.length;
    await assert.rejects(s.stat('/y'), /readdir timed out/);
    assert.equal(s.calls.length, before);
});

test('an op that answers in time resolves and is not fatal', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 200 }, { readdir: async () => [] });
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    assert.deepEqual(await s.readdir('/'), []);
    await new Promise((r) => setTimeout(r, 250));
    assert.equal(fatal, null);
});

test('ordinary errors pass through and are not fatal', { timeout: 5000 }, async () => {
    const s = new GuardedSession(
        { opTimeoutMs: 200 },
        {
            enter: async () => {
                throw new Error('session_enter failed: rc=-22');
            },
        },
    );
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    await assert.rejects(s.enter(1), /rc=-22/);
    assert.equal(fatal, null);
});

test('opTimeoutMs 0 disables the watchdog', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 0 });
    const r = await Promise.race([
        s.readdir('/').then(
            () => 'settled',
            () => 'settled',
        ),
        new Promise((res) => setTimeout(() => res('pending'), 100)),
    ]);
    assert.equal(r, 'pending');
});

test('close() after a fatal skips fd cleanup so it cannot hang', { timeout: 2000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 30 }, { open: async () => 7 });
    assert.equal(await s.openFd('/f'), 7);
    await assert.rejects(s.readdir('/'));
    await s.close();
    assert.deepEqual(s.closedFds, []);
    assert.equal(s.disposedCalled, true);
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `pnpm -C ts -F @anyfs/core build && node --test ts/packages/core/test/watchdog.test.mjs`
Expected: FAIL. `DEFAULT_OP_TIMEOUT_MS` is undefined and `this.guard is not a function`.

- [ ] **Step 3: Implement the watchdog**

In `ts/packages/core/src/session-base.ts`, insert after the two `import type` lines:

```ts
/** Default per-op watchdog (ms) — see SessionOpts.opTimeoutMs. */
export const DEFAULT_OP_TIMEOUT_MS = 60_000;

/** Options every session constructor takes. */
export interface SessionBaseOpts {
    /** Per-op watchdog (ms); 0 disables it. Default DEFAULT_OP_TIMEOUT_MS. */
    opTimeoutMs?: number | undefined;
}

/** The engine stopped answering (the op watchdog fired) or reported itself
 *  dead. The session is unusable: only a new worker — or, for the native
 *  addon, a new process — recovers. */
export class EngineFatalError extends Error {
    override name = 'EngineFatalError';
}
```

Replace the field block at the top of the class:

```ts
export abstract class AnyfsSessionBase implements AnyfsSession {
    protected disposed = false;
    protected readonly fds = new Set<LklFd>();
    private readonly fatalCbs = new Set<(e: Error) => void>();
    private fatalErr: Error | null = null;
```

with:

```ts
export abstract class AnyfsSessionBase implements AnyfsSession {
    protected disposed = false;
    protected readonly fds = new Set<LklFd>();
    /** Per-op watchdog (ms); 0 = off. */
    protected readonly opTimeoutMs: number;
    private readonly fatalCbs = new Set<(e: Error) => void>();
    private fatalErr: Error | null = null;

    constructor(opts: SessionBaseOpts = {}) {
        this.opTimeoutMs = opts.opTimeoutMs ?? DEFAULT_OP_TIMEOUT_MS;
    }
```

Replace `close()` with:

```ts
    async close(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        // Best-effort close all tracked fds — but not on a dead engine: it
        // never answers, so waiting on it would hang close() forever.
        if (!this.fatalErr) {
            for (const fd of this.fds) {
                try {
                    await this._closeFdRaw(fd);
                } catch {
                    /* best effort */
                }
            }
        }
        this.fds.clear();
        await this._dispose();
    }
```

In the "Fatal-error signalling" section, after `fireFatal()`, add:

```ts
    /** @internal — the error the session died with, or null while healthy. */
    protected get fatalError(): Error | null {
        return this.fatalErr;
    }

    /** @internal — run one engine op under the watchdog. A fatal session
     *  rejects at once instead of queueing work behind a wedged engine. An
     *  op that outlives opTimeoutMs rejects with an EngineFatalError and the
     *  session fires onFatal with the same error: a wedged kernel never
     *  answers again, so the UI must stop waiting for it. */
    protected guard<T>(op: string, run: () => Promise<T>): Promise<T> {
        if (this.fatalErr) return Promise.reject(this.fatalErr);
        const ms = this.opTimeoutMs;
        if (!(ms > 0)) return run();
        return new Promise<T>((resolve, reject) => {
            const timer = setTimeout(() => {
                const err = new EngineFatalError(
                    `${op} timed out after ${ms / 1000}s — the engine is wedged`,
                );
                // Fatal first, so listeners know the session is dead before
                // the op's caller sees the rejection.
                this.fireFatal(err);
                reject(err);
            }, ms);
            let p: Promise<T>;
            try {
                p = run();
            } catch (e) {
                clearTimeout(timer);
                reject(e);
                return;
            }
            p.then(
                (v) => {
                    clearTimeout(timer);
                    resolve(v);
                },
                (e: unknown) => {
                    clearTimeout(timer);
                    reject(e);
                },
            );
        });
    }
```

In `ts/packages/core/src/index.ts`, replace:

```ts
export { AnyfsSessionBase } from './session-base.js';
```

with:

```ts
export { AnyfsSessionBase, DEFAULT_OP_TIMEOUT_MS, EngineFatalError } from './session-base.js';
export type { SessionBaseOpts } from './session-base.js';
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm -C ts -F @anyfs/core build && pnpm -C ts -F @anyfs/core test:unit`
Expected: PASS, including the existing `session-base.test.mjs` (its `FakeSession` calls `super()`
with no args).

- [ ] **Step 5: Commit**

```bash
pnpm -C ts exec prettier --write packages/core/src/session-base.ts packages/core/src/index.ts packages/core/test/watchdog.test.mjs
git add ts/packages/core/src/session-base.ts ts/packages/core/src/index.ts ts/packages/core/test/watchdog.test.mjs
git commit -m "feat(core): add a per-op watchdog to AnyfsSessionBase

A wedged driver left enter/readdir/stat/read pending forever. guard()
rejects an op that outlives opTimeoutMs (default 60 s) with an
EngineFatalError and fires onFatal; a fatal session rejects new ops at
once, and close() no longer waits on a dead engine to close fds.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Guard `WasmSession` ops

**Files:**
- Modify: `ts/packages/core/src/wasm-session.ts`
- Modify: `ts/packages/core/src/index.ts` (`prewarm`)
- Test: `ts/packages/core/test/wasm-session.test.mjs`

- [ ] **Step 1: Write the failing test**

Create `ts/packages/core/test/wasm-session.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WasmSession } from '../dist/index.js';

/** Stands in for the module Worker: records posts and only answers when told. */
class FakeWorker extends EventTarget {
    constructor() {
        super();
        this.posted = [];
        this.terminated = false;
    }
    postMessage(m) {
        this.posted.push(m);
    }
    terminate() {
        this.terminated = true;
    }
    reply(id, result) {
        this.dispatchEvent(new MessageEvent('message', { data: { id, ok: true, result } }));
    }
}

test('a wedged readdir rejects and fires onFatal within opTimeoutMs', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 50 });
    const fatals = [];
    s.onFatal((e) => fatals.push(e.message));
    await assert.rejects(
        s.readdir('/lklmnt/x'),
        /readdir timed out after 0\.05s — the engine is wedged/,
    );
    assert.equal(fatals.length, 1);
    assert.equal(w.posted.at(-1).op, 'readdir');
    // Further ops reject at once and never reach the worker.
    const n = w.posted.length;
    await assert.rejects(s.stat('/lklmnt/x'), /readdir timed out/);
    assert.equal(w.posted.length, n);
});

test('a reply inside the window resolves normally', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 500 });
    const p = s.readdir('/');
    w.reply(w.posted[0].id, [{ name: 'a', ino: 1, kind: 'file' }]);
    assert.deepEqual(await p, [{ name: 'a', ino: 1, kind: 'file' }]);
});

test('attach is not under the op watchdog', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 20 });
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    const r = await Promise.race([
        s.attachBlob(new Blob([new Uint8Array(4)])).then(
            () => 'settled',
            () => 'settled',
        ),
        new Promise((res) => setTimeout(() => res('pending'), 100)),
    ]);
    assert.equal(r, 'pending');
    assert.equal(fatal, null);
});

test('close() after a watchdog fatal terminates the worker', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 20 });
    await assert.rejects(s.readdir('/'));
    await s.close();
    assert.equal(w.terminated, true);
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `pnpm -C ts -F @anyfs/core build && node --test ts/packages/core/test/wasm-session.test.mjs`
Expected: FAIL. The first test times out at 5 s because readdir never rejects.

- [ ] **Step 3: Implement**

In `ts/packages/core/src/wasm-session.ts`:

Change the import line to:

```ts
import { AnyfsSessionBase, type SessionBaseOpts } from './session-base.js';
```

Replace the constructor with:

```ts
    /** @internal — use mountFile() or prewarm() in index.ts. */
    constructor(worker: Worker, opts: SessionBaseOpts = {}) {
        super(opts);
        this.worker = worker;
        this.worker.addEventListener('message', this.onMessage);
        this.worker.addEventListener('error', this.onError);
    }
```

After `callRaw()`, add:

```ts
    /** An engine op: a worker call under the base watchdog. Attach and boot
     *  use call() directly — the provider's attach timeout bounds those. */
    private op<T>(op: string, args: unknown = {}): Promise<T> {
        return this.guard(op, () => this.call<T>(op, args));
    }
```

Then change `this.call` to `this.op` in exactly these methods: `enter`, `listParts`, `meta`, `readdir`,
`stat`, `statFollow`, `readlink`, `realpath`, `readKernelFile`, `_openFdRaw`, `_readFdRaw`,
`_closeFdRaw`. Leave `attachBlob`, `attachUrl` and `callRaw` on `call`. For example:

```ts
    async readdir(path: string): Promise<DirEntry[]> {
        return this.op<DirEntry[]>('readdir', { path });
    }
```

In `ts/packages/core/src/index.ts` `prewarm()`, replace:

```ts
    const session = new WasmSession(worker);
```

with:

```ts
    const session = new WasmSession(worker, { opTimeoutMs: opts.opTimeoutMs });
```

`BrowserMountOpts extends SessionOpts`, so the option belongs in `SessionOpts`. In
`ts/packages/core/src/types.ts`, inside `SessionOpts` after `attachTimeoutMs`, add:

```ts
    /** Watchdog (ms) for every engine op after attach — enter, listParts,
     *  readdir, stat, reads… An op that doesn't finish in time rejects and the
     *  session fires onFatal, so a wedged filesystem driver can't spin the UI
     *  forever. Default 60000. Set 0 to disable. */
    opTimeoutMs?: number;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm -C ts -F @anyfs/core build && pnpm -C ts -F @anyfs/core test:unit`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
pnpm -C ts exec prettier --write packages/core/src/wasm-session.ts packages/core/src/index.ts packages/core/src/types.ts packages/core/test/wasm-session.test.mjs
git add ts/packages/core/src/wasm-session.ts ts/packages/core/src/index.ts ts/packages/core/src/types.ts ts/packages/core/test/wasm-session.test.mjs
git commit -m "feat(core): run WasmSession ops under the op watchdog

Every post-attach worker op goes through guard(); attach and boot stay
under the provider's attach timeout. New SessionOpts.opTimeoutMs.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Guard `NativeSession` ops

**Files:**
- Modify: `ts/packages/core/src/native-session.ts`
- Modify: `ts/packages/core/src/index.ts` (`prewarmNative`)
- Test: `ts/packages/core/test/native-watchdog.test.mjs`

- [ ] **Step 1: Write the failing test**

Create `ts/packages/core/test/native-watchdog.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NativeSession } from '../dist/index.js';

const never = () => new Promise(() => {});
const OPS = [
    'available',
    'init',
    'diskOpen',
    'diskClose',
    'diskListJson',
    'diskMetaJson',
    'diskEnter',
    'readdirJson',
    'lstatJson',
    'statJson',
    'realpath',
    'readlink',
    'startProxy',
    'stopProxy',
    'fileOpen',
    'pread',
    'fileClose',
];

/** A bridge whose ops answer from `impl`, or never. */
function fakeBridge(impl = {}) {
    const calls = [];
    const bridge = {};
    for (const name of OPS) {
        bridge[name] = (...args) => {
            calls.push(name);
            return (impl[name] ?? never)(...args);
        };
    }
    if (impl.onFatal) bridge.onFatal = impl.onFatal;
    return { bridge, calls };
}

test('a wedged readdir rejects and fires onFatal within opTimeoutMs', { timeout: 5000 }, async () => {
    const { bridge, calls } = fakeBridge({ diskOpen: async () => 0 });
    const s = new NativeSession(bridge, { opTimeoutMs: 50 });
    await s.attachPath('/img');
    const fatals = [];
    s.onFatal((e) => fatals.push(e.message));
    await assert.rejects(
        s.readdir('/lklmnt/a'),
        /readdir timed out after 0\.05s — the engine is wedged/,
    );
    assert.equal(fatals.length, 1);
    const n = calls.length;
    await assert.rejects(s.stat('/lklmnt/a'), /readdir timed out/);
    assert.equal(calls.length, n);
});

test('close() after a watchdog fatal does not wait on the wedged engine', { timeout: 2000 }, async () => {
    const { bridge, calls } = fakeBridge({ diskOpen: async () => 0, fileOpen: async () => 5 });
    const s = new NativeSession(bridge, { opTimeoutMs: 30 });
    await s.attachPath('/img');
    assert.equal(await s.openFd('/lklmnt/a/f'), 5);
    await assert.rejects(s.readdir('/lklmnt/a'));
    await s.close();
    assert.ok(!calls.includes('fileClose'), 'must not close fds on a dead engine');
    assert.ok(!calls.includes('diskClose'), 'must not close the disk on a dead engine');
});

test('the timer starts when an op reaches the engine, not while it queues', { timeout: 5000 }, async () => {
    const { bridge } = fakeBridge({
        diskOpen: async () => 0,
        readdirJson: () => new Promise((r) => setTimeout(() => r('[]'), 80)),
        lstatJson: () => new Promise((r) => setTimeout(() => r(JSON.stringify({ ino: 1 })), 60)),
    });
    const s = new NativeSession(bridge, { opTimeoutMs: 100 });
    await s.attachPath('/img');
    const a = s.readdir('/a'); // 80 ms
    const b = s.stat('/b'); // queued 80 ms, then 60 ms: 140 ms in total, 60 ms on the engine
    assert.deepEqual(await a, []);
    assert.deepEqual(await b, { ino: 1 });
});

test('a host-reported engine failure still fires onFatal', () => {
    let push;
    const { bridge } = fakeBridge({
        onFatal: (cb) => {
            push = cb;
            return () => {};
        },
    });
    const s = new NativeSession(bridge);
    const fatals = [];
    s.onFatal((e) => fatals.push([e.name, e.message]));
    push('QEMU thread did not finish read within 120000 ms');
    assert.deepEqual(fatals, [
        ['EngineFatalError', 'anyfs-native engine failed: QEMU thread did not finish read within 120000 ms'],
    ]);
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `pnpm -C ts -F @anyfs/core build && node --test ts/packages/core/test/native-watchdog.test.mjs`
Expected: FAIL. The first test times out, and the last fails because the error name is `Error`.

- [ ] **Step 3: Implement**

In `ts/packages/core/src/native-session.ts`:

Change the import line to:

```ts
import { AnyfsSessionBase, EngineFatalError, type SessionBaseOpts } from './session-base.js';
```

Replace this field block and the constructor:

```ts
    // Set once the host reports the engine wedged: every pending and future
    // call would hang, so dispose must not wait on them.
    private engineFailed = false;
    private readonly unsubscribeFatal: (() => void) | null;

    constructor(bridge: AnyfsNativeBridge) {
        super();
        this.bridge = bridge;
        this.unsubscribeFatal =
            bridge.onFatal?.((reason) => {
                this.engineFailed = true;
                this.fireFatal(new Error(`anyfs-native engine failed: ${reason}`));
            }) ?? null;
    }
```

with:

```ts
    private readonly unsubscribeFatal: (() => void) | null;

    constructor(bridge: AnyfsNativeBridge, opts: SessionBaseOpts = {}) {
        super(opts);
        this.bridge = bridge;
        // The host reports a wedged engine (the QEMU thread missed its own
        // watchdog). Either way — that or our op watchdog — the session is
        // fatal and dispose must not wait on the engine.
        this.unsubscribeFatal =
            bridge.onFatal?.((reason) => {
                this.fireFatal(new EngineFatalError(`anyfs-native engine failed: ${reason}`));
            }) ?? null;
    }
```

After `chain()`, add:

```ts
    /** An engine op: serialized, then run under the base watchdog. The timer
     *  starts when the op reaches the engine, not while it waits its turn. */
    private op<T>(op: string, fn: () => Promise<T>): Promise<T> {
        return this.chain(() => this.guard(op, fn));
    }
```

Change `this.chain(` to `this.op('<name>', ` in these methods, using these names: `enter` → `'enter'`,
`listParts` → `'listParts'`, `meta` → `'meta'`, `readdir` → `'readdir'`, `stat` → `'stat'`,
`statFollow` → `'statFollow'`, `readlink` → `'readlink'`, `realpath` → `'realpath'`, `_openFdRaw` →
`'open'`, `_readFdRaw` → `'read'`, `_closeFdRaw` → `'close'`. Leave `attachPath` and `attachUrl` on
`chain`. For example:

```ts
    async readdir(path: string): Promise<DirEntry[]> {
        return this.op('readdir', async () => JSON.parse(await this.bridge.readdirJson(path)));
    }
```

In `_dispose()`, replace:

```ts
        // A wedged engine never settles the in-flight op or a close call.
        if (this.engineFailed) return;
```

with:

```ts
        // A wedged engine never settles the in-flight op or a close call.
        if (this.fatalError) return;
```

In `ts/packages/core/src/index.ts` `prewarmNative()`, replace the signature and construction:

```ts
export async function prewarmNative(
    opts: Pick<SessionOpts, 'memMb' | 'loglevel'> = {},
): Promise<NativeSession | null> {
```

```ts
    const session = new NativeSession(bridge);
```

with:

```ts
export async function prewarmNative(
    opts: Pick<SessionOpts, 'memMb' | 'loglevel' | 'opTimeoutMs'> = {},
): Promise<NativeSession | null> {
```

```ts
    const session = new NativeSession(bridge, { opTimeoutMs: opts.opTimeoutMs });
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `pnpm -C ts -F @anyfs/core build && pnpm -C ts -F @anyfs/core test:unit`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
pnpm -C ts exec prettier --write packages/core/src/native-session.ts packages/core/src/index.ts packages/core/test/native-watchdog.test.mjs
git add ts/packages/core/src/native-session.ts ts/packages/core/src/index.ts ts/packages/core/test/native-watchdog.test.mjs
git commit -m "feat(core): run NativeSession ops under the op watchdog

Each serialized op is guarded once it reaches the engine. A watchdog or
host-reported fatal now both skip waiting on the wedged addon at
dispose (engineFailed is folded into the base fatal state).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `NodeWasmSession`: watchdog, abort → fatal, read-only open

**Files:**
- Modify: `ts/packages/core/src/wasm-api.ts`
- Modify: `ts/packages/core/src/node-wasm-session.ts`
- Modify: `ts/packages/core/src/boot.ts`
- Modify: `ts/packages/core/src/node.ts`
- Modify: `ts/packages/core/src/module.ts`
- Modify: `ts/packages/core/src/index.ts`
- Test: `ts/packages/core/test/node-wasm-session.test.mjs`

- [ ] **Step 1: Write the failing test**

Create `ts/packages/core/test/node-wasm-session.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NodeWasmSession } from '../dist/index.js';
import { bootNodeKernel } from '../dist/node.js';

/** Just enough of the emscripten module for WasmApi: a heap, a bump
 *  allocator, and an "API thread" that answers only when told to. */
function fakeModule() {
    const heap = new ArrayBuffer(1 << 20);
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const M = {
        HEAPU8: new Uint8Array(heap),
        HEAP32: new Int32Array(heap),
        HEAPU32: new Uint32Array(heap),
        top: 1024,
        submitted: [],
        autoAnswer: false,
        _malloc(n) {
            const p = M.top;
            M.top += (n + 7) & ~7;
            return p;
        },
        _free() {},
        ccall(name, _ret, _types, [req]) {
            assert.equal(name, 'anyfs_ts_api_submit');
            M.submitted.push(req);
            if (M.autoAnswer) queueMicrotask(() => M.answer(0));
            return 0;
        },
        /** The API thread finishes the oldest request with return value `ret`. */
        answer(ret) {
            const w = M.submitted.shift() >> 2;
            M.HEAP32[w + 2] = ret;
            M.anyfsApiDone(M.HEAP32[w + 1]);
        },
        lengthBytesUTF8: (s) => enc.encode(s).length,
        stringToUTF8(s, p, max) {
            const b = enc.encode(s).subarray(0, max - 1);
            M.HEAPU8.set(b, p);
            M.HEAPU8[p + b.length] = 0;
        },
        UTF8ToString(p, max = Infinity) {
            let e = p;
            while (e - p < max && M.HEAPU8[e] !== 0) e++;
            return dec.decode(M.HEAPU8.subarray(p, e));
        },
    };
    return M;
}

test('a wedged op rejects and fires onFatal within opTimeoutMs', { timeout: 5000 }, async () => {
    const s = new NodeWasmSession(fakeModule(), { opTimeoutMs: 50 });
    const fatals = [];
    s.onFatal((e) => fatals.push(e.message));
    await assert.rejects(s.readdir('/work'), /readdir timed out after 0\.05s — the engine is wedged/);
    assert.equal(fatals.length, 1);
    await s.close(); // must not wait on the wedged API thread
});

test('readOnly opens the image with ANYFS_SESSION_READONLY', { timeout: 5000 }, async () => {
    const M = fakeModule();
    const s = new NodeWasmSession(M, { readOnly: true });
    const p = s.attachPath('/work/disk.img');
    const w = M.submitted[0] >> 2;
    assert.equal(M.HEAP32[w], 3); // ApiOp.SESSION_OPEN
    assert.equal(M.HEAP32[w + 4], 1); // flags
    M.answer(0);
    await p;
});

test('a module abort rejects pending ops and fires onFatal', { timeout: 5000 }, async () => {
    const M = fakeModule();
    let onAbort;
    const factory = async (opts) => {
        onAbort = opts.onAbort;
        return M;
    };
    M.autoAnswer = true; // KERNEL_INIT succeeds at once
    assert.equal(await bootNodeKernel('/nonexistent', factory), M);
    M.autoAnswer = false; // from here on, ops wedge until answered
    const s = new NodeWasmSession(M, { opTimeoutMs: 0 });
    const fatals = [];
    s.onFatal((e) => fatals.push([e.name, e.message]));
    const pending = s.readdir('/work');
    onAbort('native code called abort()');
    await assert.rejects(pending, /wasm module aborted: native code called abort\(\)/);
    assert.deepEqual(fatals, [['EngineFatalError', 'wasm module aborted: native code called abort()']]);
    await assert.rejects(s.stat('/work'), /wasm module aborted/);
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `pnpm -C ts -F @anyfs/core build && node --test ts/packages/core/test/node-wasm-session.test.mjs`
Expected: FAIL. `bootNodeKernel` is not exported, readdir doesn't time out, and `readOnly` is ignored
(flags `0`).

- [ ] **Step 3: Make `WasmApi` failable**

In `ts/packages/core/src/wasm-api.ts`, replace the whole `export class WasmApi { … }` with:

```ts
export class WasmApi {
    private readonly M: WasmApiModule;
    private readonly pending = new Map<number, { resolve: () => void; reject: (e: Error) => void }>();
    private readonly failCbs = new Set<(e: Error) => void>();
    private failure: Error | null = null;
    private nextId = 1;

    constructor(M: WasmApiModule) {
        this.M = M;
        M.anyfsApiDone = (id: number) => {
            const p = this.pending.get(id);
            this.pending.delete(id);
            p?.resolve();
        };
    }

    /** The error the module died with, or null while it is alive. */
    get failed(): Error | null {
        return this.failure;
    }

    /** Mark the module dead (it aborted): every pending and future call
     *  rejects with `err`, and onFail listeners run once. */
    fail(err: Error): void {
        if (this.failure) return;
        this.failure = err;
        for (const p of this.pending.values()) p.reject(err);
        this.pending.clear();
        for (const cb of this.failCbs) {
            try {
                cb(err);
            } catch {
                /* a listener throwing must not block the others */
            }
        }
        this.failCbs.clear();
    }

    /** Run `cb` once when the module dies (at once if it already has).
     *  Returns an unsubscribe fn. */
    onFail(cb: (err: Error) => void): () => void {
        if (this.failure) {
            cb(this.failure);
            return () => {};
        }
        this.failCbs.add(cb);
        return () => this.failCbs.delete(cb);
    }

    /** Run `op` on the API thread and resolve with its int result. String
     *  args are copied into the heap for the duration of the call. */
    async call(op: number, args: ReadonlyArray<number | string> = []): Promise<number> {
        if (this.failure) throw this.failure;
        if (args.length > MAX_ARGS) throw new Error(`wasm op ${op}: too many args`);
        const M = this.M;
        const req = M._malloc(REQ_BYTES);
        const strings: number[] = [];
        try {
            const w = req >> 2;
            M.HEAP32.fill(0, w, w + REQ_WORDS);
            const id = this.nextId++;
            M.HEAP32[w] = op;
            M.HEAP32[w + 1] = id;
            args.forEach((a, i) => {
                let v = a;
                if (typeof a === 'string') {
                    v = this.allocString(a);
                    strings.push(v);
                }
                M.HEAP32[w + 3 + i] = (v as number) | 0;
            });
            const done = new Promise<void>((resolve, reject) =>
                this.pending.set(id, { resolve, reject }),
            );
            if (M.ccall('anyfs_ts_api_submit', 'number', ['number'], [req]) !== 0) {
                this.pending.delete(id);
                throw new Error('anyfs_ts_api_submit: API thread unavailable');
            }
            await done;
            return M.HEAP32[w + 2] ?? -1;
        } finally {
            for (const p of strings) this.free(p);
            this.free(req);
        }
    }

    /** For ops that write into a (buf, cap) pair as their last two args and
     *  return the byte count, or -needed when cap is too small: grow and
     *  retry, then decode the bytes as UTF-8. */
    async callStringOut(
        op: number,
        args: ReadonlyArray<number | string>,
        name: string,
        initialCap = 8192,
    ): Promise<string> {
        const M = this.M;
        let cap = initialCap;
        for (let i = 0; i < 6; i++) {
            const buf = M._malloc(cap);
            try {
                const n = await this.call(op, [...args, buf, cap]);
                if (n >= 0) return M.UTF8ToString(buf, n);
                const need = -n;
                if (need <= cap) throw new Error(`${name}: rc=${n}`);
                cap = Math.max(need + 256, cap * 2);
            } finally {
                this.free(buf);
            }
        }
        throw new Error(`${name}: keeps requesting more buffer`);
    }

    /** callStringOut + JSON.parse. */
    async callJsonOut(
        op: number,
        args: ReadonlyArray<number | string>,
        name: string,
    ): Promise<unknown> {
        return JSON.parse(await this.callStringOut(op, args, name));
    }

    /** The API thread's last error message (set by the op that just failed),
     *  or '' if there is none. */
    async lastError(): Promise<string> {
        const M = this.M;
        const cap = 512;
        const buf = M._malloc(cap);
        try {
            const n = await this.call(ApiOp.LAST_ERROR, [buf, cap]);
            return n > 0 ? M.UTF8ToString(buf, n) : '';
        } finally {
            this.free(buf);
        }
    }

    /** Free heap memory — unless the module is dead: its heap can't be
     *  trusted, and calling into an aborted module throws. */
    free(p: number): void {
        if (!this.failure) this.M._free(p);
    }

    private allocString(s: string): number {
        const M = this.M;
        const n = M.lengthBytesUTF8(s) + 1;
        const p = M._malloc(n);
        M.stringToUTF8(s, p, n);
        return p;
    }
}
```

- [ ] **Step 4: Rewrite `NodeWasmSession`**

Replace the whole content of `ts/packages/core/src/node-wasm-session.ts` with:

```ts
import type { AnyfsModule } from './module.js';
import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import { AnyfsSessionBase, type SessionBaseOpts } from './session-base.js';
import { ApiOp, wasmApiFor, type WasmApi } from './wasm-api.js';

export interface NodeWasmSessionOpts extends SessionBaseOpts {
    /** Open the image read-only (ANYFS_SESSION_READONLY), as the browser
     *  worker does. Default false. */
    readOnly?: boolean | undefined;
}

/**
 * Node wasm session — the bundle runs on Node's main thread (no Worker).
 *
 * Every op goes through the bundle's API thread (see wasm-api.ts): this
 * thread owns the module and must stay free to serve the NODEFS calls the
 * QEMU thread proxies to it.
 *
 * The caller owns kernel boot (bootModule) and passes the live module to the
 * constructor. `attachPath(fsPath)` opens the disk image via the C glue;
 * NODEFS mount of the host directory must already be set up through boot's
 * `preRun` hook.
 *
 * The module is process-global. When it aborts (kernel panic, wasm trap)
 * every session on it fires onFatal, and recovery means a new process.
 */
export class NodeWasmSession extends AnyfsSessionBase {
    private readonly M: AnyfsModule;
    private readonly api: WasmApi;
    private readonly openFlags: number;
    private readonly unsubscribeFail: () => void;
    private handle = -1;

    /** @internal — use mountNodeFile() in node.ts or construct directly. */
    constructor(M: AnyfsModule, opts: NodeWasmSessionOpts = {}) {
        super(opts);
        this.M = M;
        this.api = wasmApiFor(M);
        this.openFlags = opts.readOnly ? 1 : 0;
        this.unsubscribeFail = this.api.onFail((err) => this.fireFatal(err));
    }

    // ── Attach ─────────────────────────────────────────

    async attachPath(fsPath: string): Promise<void> {
        this.check();
        if (this.handle >= 0) throw new Error('NodeWasmSession: already attached');
        const h = await this.api.call(ApiOp.SESSION_OPEN, [fsPath, this.openFlags]);
        if (h < 0) {
            const why = await this.api.lastError();
            throw new Error(`session_open(${fsPath}) failed: ${why || `rc=${h}`}`);
        }
        this.handle = h;
    }

    async attachBlob(_blob: Blob): Promise<void> {
        throw new Error('NodeWasmSession: attachBlob(Blob) not supported; use attachPath(string)');
    }

    async attachUrl(_url: string, _name?: string): Promise<void> {
        throw new Error('NodeWasmSession: attachUrl not supported in Node wasm mode');
    }

    // ── Partition / mount ──────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        this.check();
        return this.guard('enter', async () => {
            const cap = 128;
            const buf = this.M._malloc(cap);
            try {
                const rc = await this.api.call(ApiOp.SESSION_ENTER, [
                    this.handle,
                    part,
                    flags,
                    buf,
                    cap,
                ]);
                if (rc !== 0) {
                    const why = await this.api.lastError();
                    throw new Error(`session_enter failed: rc=${rc}${why ? `: ${why}` : ''}`);
                }
                return this.M.UTF8ToString(buf);
            } finally {
                this.api.free(buf);
            }
        });
    }

    async listParts(): Promise<SessionPartInfo[]> {
        this.check();
        return this.guard(
            'listParts',
            async () =>
                (await this.api.callJsonOut(
                    ApiOp.SESSION_LIST,
                    [this.handle],
                    'session_list_json',
                )) as SessionPartInfo[],
        );
    }

    async meta(): Promise<SessionMeta> {
        this.check();
        return this.guard(
            'meta',
            async () =>
                (await this.api.callJsonOut(
                    ApiOp.SESSION_META,
                    [this.handle],
                    'session_meta_json',
                )) as SessionMeta,
        );
    }

    // ── Filesystem ops ─────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        this.check();
        return this.guard(
            'readdir',
            async () =>
                (await this.api.callJsonOut(ApiOp.READDIR, [path], 'readdir_json')) as DirEntry[],
        );
    }

    async stat(path: string): Promise<Stat> {
        this.check();
        return this.guard(
            'stat',
            async () => (await this.api.callJsonOut(ApiOp.LSTAT, [path], 'lstat_json')) as Stat,
        );
    }

    async statFollow(path: string): Promise<Stat> {
        this.check();
        return this.guard(
            'statFollow',
            async () => (await this.api.callJsonOut(ApiOp.STAT, [path], 'stat_json')) as Stat,
        );
    }

    async readlink(path: string): Promise<string> {
        this.check();
        return this.guard('readlink', () => this.pathOut(ApiOp.READLINK, path, 'readlink'));
    }

    async realpath(path: string): Promise<string> {
        this.check();
        return this.guard('realpath', () => this.pathOut(ApiOp.REALPATH, path, 'realpath'));
    }

    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        this.check();
        return this.guard('readKernelFile', () =>
            this.api.callStringOut(ApiOp.READ_KERNEL_FILE, [path], 'readKernelFile', 4096),
        );
    }

    onProgress(_cb: (step: string) => void): () => void {
        // Direct calls have no progress events — return a no-op unsubscriber.
        return () => undefined;
    }

    // ── Internal fd ops ────────────────────────────────

    /** @internal */
    protected async _openFdRaw(path: string): Promise<LklFd> {
        return this.guard('open', async () => {
            const fd = await this.api.call(ApiOp.OPEN, [path, 0]);
            if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
            return fd;
        });
    }

    /** @internal */
    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        return this.guard('read', async () => {
            const buf = this.M._malloc(length);
            try {
                const off = BigInt(offset);
                const lo = Number(off & 0xffffffffn) | 0;
                const hi = Number((off >> 32n) & 0xffffffffn) | 0;
                const n = await this.api.call(ApiOp.PREAD, [fd, buf, length, lo, hi]);
                if (n < 0) throw new Error(`pread failed: ${n}`);
                return this.M.HEAPU8.slice(buf, buf + n);
            } finally {
                this.api.free(buf);
            }
        });
    }

    /** @internal */
    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        return this.guard('close', async () => {
            const rc = await this.api.call(ApiOp.CLOSE, [fd]);
            if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
        });
    }

    /** @internal */
    protected async _dispose(): Promise<void> {
        this.unsubscribeFail();
        // A wedged or aborted engine never answers session_close.
        if (this.handle >= 0 && !this.fatalError) {
            try {
                await this.api.call(ApiOp.SESSION_CLOSE, [this.handle]);
            } catch {
                /* best effort */
            }
        }
        this.handle = -1;
    }

    // PATH_MAX is 4096 on Linux; longer can't be represented anyway.
    private async pathOut(op: number, path: string, name: string): Promise<string> {
        const cap = 4096;
        const buf = this.M._malloc(cap);
        try {
            const n = await this.api.call(op, [path, buf, cap]);
            if (n < 0) throw new Error(`${name} rc=${n}`);
            return this.M.UTF8ToString(buf, n);
        } finally {
            this.api.free(buf);
        }
    }
}
```

- [ ] **Step 5: Hook `onAbort` in boot, add `bootNodeKernel`**

In `ts/packages/core/src/module.ts`, add `onAbort` to the factory options:

```ts
export type AnyfsModuleFactory = (opts?: {
    preRun?: Array<(m: AnyfsModule) => void>;
    locateFile?: (path: string, prefix: string) => string;
    print?: (msg: string) => void;
    printErr?: (msg: string) => void;
    onAbort?: (what: unknown) => void;
}) => Promise<AnyfsModule>;
```

Replace the whole content of `ts/packages/core/src/boot.ts` with:

```ts
import { NodeWasmSession, type NodeWasmSessionOpts } from './node-wasm-session.js';
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import { EngineFatalError } from './session-base.js';
import { ApiOp, wasmApiFor } from './wasm-api.js';

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
        let live: AnyfsModule | null = null;
        // abort() — a kernel panic, a wasm trap, OOM — calls this on the
        // module-owning thread (emscripten proxies it from pthreads). Fail
        // the API so pending and future ops reject and every session fires
        // onFatal; the module stays dead for the life of the process.
        const onAbort = (what: unknown) => {
            if (live) {
                wasmApiFor(live).fail(new EngineFatalError(`wasm module aborted: ${String(what)}`));
            }
        };
        const M = await args.factory({ preRun: args.preRun, onAbort });
        live = M;
        if (!g_kernelInitialised) {
            const rc = await wasmApiFor(M).call(ApiOp.KERNEL_INIT, [args.memMb, args.loglevel]);
            if (rc !== 0) throw new Error(`anyfs_ts_kernel_init failed: ${rc}`);
            g_kernelInitialised = true;
        }
        return M;
    })();
    return g_modulePromise;
}

/** Open a session for the given disk image path on a booted module. NODEFS
 *  mount of the host directory must already be set up (via boot's `preRun`
 *  hook). */
export async function openNodeSession(
    M: AnyfsModule,
    fsPath: string,
    opts: NodeWasmSessionOpts = {},
): Promise<NodeWasmSession> {
    const session = new NodeWasmSession(M, opts);
    try {
        await session.attachPath(fsPath);
    } catch (err) {
        await session.close();
        throw err;
    }
    return session;
}

export async function haltKernel(): Promise<void> {
    if (!g_modulePromise) return;
    const M = await g_modulePromise;
    await wasmApiFor(M).call(ApiOp.KERNEL_HALT);
    g_modulePromise = null;
    g_kernelInitialised = false;
}
```

Replace the whole content of `ts/packages/core/src/node.ts` with:

```ts
/** Node-only entry — uses NODEFS. Browser code should NOT import this. */
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import type { SessionOpts } from './types.js';
import { bootModule, openNodeSession, haltKernel as halt } from './boot.js';

export interface NodeMountOpts extends SessionOpts {
    /** Open the image read-only (ANYFS_SESSION_READONLY), as the browser
     *  worker does. Default false. */
    readOnly?: boolean;
}

/** Boot the process-global wasm kernel with host directory `hostDir`
 *  mounted at /work (NODEFS). Idempotent: later calls return the same
 *  module. */
export async function bootNodeKernel(
    hostDir: string,
    factory: AnyfsModuleFactory,
    opts: SessionOpts = {},
): Promise<AnyfsModule> {
    return bootModule({
        factory,
        memMb: opts.memMb ?? 64,
        loglevel: opts.loglevel ?? 0,
        preRun: [
            (m: AnyfsModule) => {
                if (!m.NODEFS) throw new Error('NODEFS not exported');
                m.FS.mkdir('/work');
                m.FS.mount(m.NODEFS, { root: hostDir }, '/work');
            },
        ],
    });
}

export async function mountNodeFile(
    hostPath: string,
    factory: AnyfsModuleFactory,
    opts: NodeMountOpts = {},
) {
    const { default: path } = await import('node:path');
    const M = await bootNodeKernel(path.dirname(hostPath), factory, opts);
    return openNodeSession(M, `/work/${path.basename(hostPath)}`, {
        opTimeoutMs: opts.opTimeoutMs,
        readOnly: opts.readOnly,
    });
}

export { openNodeSession };
export const haltKernel = halt;
```

In `ts/packages/core/src/index.ts`, replace:

```ts
export { NodeWasmSession } from './node-wasm-session.js';
```

with:

```ts
export { NodeWasmSession } from './node-wasm-session.js';
export type { NodeWasmSessionOpts } from './node-wasm-session.js';
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `pnpm -C ts -F @anyfs/core build && pnpm -C ts -F @anyfs/core test:unit`
Expected: PASS (all core unit test files).

- [ ] **Step 7: Smoke the real bundle**

Run: `cd ts/packages/core && node test/smoke.node.mjs single; cd -`
Expected: the smoke test still passes. It talks to the ABI directly, so this checks that the bundle is
intact. Then run `cd ts/packages/core && node test/api.node.mjs; cd -` (it uses `mountNodeFile`).
Expected: it passes as before. If it needs an image that is missing, record that and move on: the
fake-module unit tests above cover the TS changes.

- [ ] **Step 8: Commit**

```bash
pnpm -C ts exec prettier --write packages/core/src/wasm-api.ts packages/core/src/node-wasm-session.ts packages/core/src/boot.ts packages/core/src/node.ts packages/core/src/module.ts packages/core/src/index.ts packages/core/test/node-wasm-session.test.mjs
git add ts/packages/core/src ts/packages/core/test/node-wasm-session.test.mjs
git commit -m "feat(core): NodeWasmSession watchdog, abort-to-fatal, read-only open

A wasm abort (kernel panic, trap) now fails the module's WasmApi:
pending and future ops reject and every NodeWasmSession fires onFatal
instead of surfacing as an unhandled error. Ops run under the op
watchdog. bootNodeKernel() splits boot from attach; readOnly opens the
image with ANYFS_SESSION_READONLY like the browser worker.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Provider wiring and the native engine-fatal hint

**Files:**
- Modify: `ts/packages/react/src/provider.tsx:166-185` (`bootSession`)
- Modify: `ts/examples/vite-demo/src/components/DiskView.tsx:19,107-112`

- [ ] **Step 1: Forward `opTimeoutMs`**

In `ts/packages/react/src/provider.tsx` `bootSession.current`, replace the native branch's options:

```ts
            return prewarmNative({
                ...(mountOpts?.memMb !== undefined ? { memMb: mountOpts.memMb } : {}),
                ...(mountOpts?.loglevel !== undefined ? { loglevel: mountOpts.loglevel } : {}),
            }).then((s) => {
```

with:

```ts
            return prewarmNative({
                ...(mountOpts?.memMb !== undefined ? { memMb: mountOpts.memMb } : {}),
                ...(mountOpts?.loglevel !== undefined ? { loglevel: mountOpts.loglevel } : {}),
                ...(mountOpts?.opTimeoutMs !== undefined
                    ? { opTimeoutMs: mountOpts.opTimeoutMs }
                    : {}),
            }).then((s) => {
```

and after `if (mountOpts?.forceFstype !== undefined) opts.forceFstype = mountOpts.forceFstype;` add:

```ts
        if (mountOpts?.opTimeoutMs !== undefined) opts.opTimeoutMs = mountOpts.opTimeoutMs;
```

- [ ] **Step 2: Tell native users how to recover**

In `ts/examples/vite-demo/src/components/DiskView.tsx`, change line 19 to also read `mode`:

```tsx
    const { session, mountPath, status, step, error, mode } = useAnyfsDisk();
```

Replace the `status === 'error'` block:

```tsx
    if (status === 'error')
        return (
            <div className="flex-1 flex items-center justify-center text-base text-red-500 dark:text-red-400">
                Error: {error?.message}
            </div>
        );
```

with:

```tsx
    if (status === 'error')
        return (
            <div className="flex-1 flex flex-col items-center justify-center gap-2 text-base text-red-500 dark:text-red-400">
                <div>Error: {error?.message}</div>
                {mode === 'native' && error?.name === 'EngineFatalError' && (
                    // The addon's kernel is process-global: a wedged op holds it
                    // until the app restarts.
                    <div
                        className="text-sm text-zinc-600 dark:text-zinc-400"
                        data-testid="engine-fatal-hint"
                    >
                        The native engine stopped responding. Switch to the wasm engine in
                        Settings, or restart the app.
                    </div>
                )}
            </div>
        );
```

- [ ] **Step 3: Build to type-check**

Run: `pnpm -C ts build && pnpm -C ts -F vite-demo build`
Expected: both succeed with no type errors.

- [ ] **Step 4: Commit**

```bash
pnpm -C ts exec prettier --write packages/react/src/provider.tsx examples/vite-demo/src/components/DiskView.tsx
git add ts/packages/react/src/provider.tsx ts/examples/vite-demo/src/components/DiskView.tsx
git commit -m "feat(react,vite-demo): forward opTimeoutMs; explain native engine fatals

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Robustness scaffolding: paths, PRNG, checksums, tree, tools

**Files:**
- Create: `ts/tests/robustness/lib/paths.mjs`
- Create: `ts/tests/robustness/corpus/rng.mjs`
- Create: `ts/tests/robustness/corpus/checksum.mjs`
- Create: `ts/tests/robustness/corpus/tree.mjs`
- Create: `ts/tests/robustness/corpus/tools.mjs`
- Test: `ts/tests/robustness/test/corpus.test.mjs`

- [ ] **Step 1: Write the failing test**

Create `ts/tests/robustness/test/corpus.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { crc32c, crc32cRaw } from '../corpus/checksum.mjs';
import { randInt, rng } from '../corpus/rng.mjs';

test('crc32c matches the standard check value', () => {
    assert.equal(crc32c(Buffer.from('123456789')), 0xe3069283);
    assert.equal(crc32cRaw(0xffffffff, Buffer.from('123456789')), ~0xe3069283 >>> 0);
});

test('rng is deterministic per seed', () => {
    const a = rng(1);
    const b = rng(1);
    const c = rng(2);
    const sa = [a(), a(), a()];
    assert.deepEqual([b(), b(), b()], sa);
    assert.notDeepEqual([c(), c(), c()], sa);
    const next = rng(7);
    for (let i = 0; i < 1000; i++) {
        const n = randInt(next, 10);
        assert.ok(Number.isInteger(n) && n >= 0 && n < 10);
    }
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `node --test ts/tests/robustness/test/corpus.test.mjs`
Expected: FAIL with `Cannot find module '.../corpus/checksum.mjs'`.

- [ ] **Step 3: Create the modules**

`ts/tests/robustness/lib/paths.mjs`:

```js
/** Paths shared by the robustness corpus, harness and tests. */
import { homedir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROBUSTNESS_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '..');
export const TS_DIR = resolve(ROBUSTNESS_DIR, '../..');

/** Everything generated or downloaded lives here — never /tmp (a small tmpfs). */
export const CACHE_DIR =
    process.env.ANYFS_ROBUSTNESS_DIR ??
    join(process.env.XDG_CACHE_HOME ?? join(homedir(), '.cache'), 'anyfs-robustness');
export const GENERATED_DIR = join(CACHE_DIR, 'generated');
export const CASES_JSON = join(GENERATED_DIR, 'cases.json');
export const SYZBOT_DIR = join(CACHE_DIR, 'syzbot');
export const LOG_DIR = join(CACHE_DIR, 'logs');
export const SCRATCH_DIR = join(CACHE_DIR, 'test-scratch');

export const SYZBOT_MANIFEST = join(ROBUSTNESS_DIR, 'syzbot.json');
export const CORE_DIST = join(TS_DIR, 'packages/core/dist');
export const WASM_NODE_BUNDLE = join(TS_DIR, 'packages/core/wasm/anyfs.node.mjs');
export const NATIVE_ADDON = join(TS_DIR, 'packages/anyfs-native/build/Release/anyfs_native.node');

/** report-<backend>.json for a full run; a --only run writes -partial. */
export const reportPath = (backend, partial = false) =>
    join(CACHE_DIR, `report-${backend}${partial ? '-partial' : ''}.json`);
```

`ts/tests/robustness/corpus/rng.mjs`:

```js
/** mulberry32: a tiny seeded PRNG. Same seed, same stream, on every host. */
export function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/** An integer in [0, n) from `next`. */
export const randInt = (next, n) => Math.floor(next() * n);
```

`ts/tests/robustness/corpus/checksum.mjs`:

```js
/** CRC32C (Castagnoli), as ext4, btrfs and xfs use it. */
const TABLE = (() => {
    const t = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0x82f63b78 : c >>> 1;
        t[i] = c >>> 0;
    }
    return t;
})();

/** The kernel's crc32c(): no final inversion. ext4 stores this form. */
export function crc32cRaw(seed, buf) {
    let c = seed >>> 0;
    for (let i = 0; i < buf.length; i++) c = TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return c >>> 0;
}

/** Standard CRC32C (init ~0, final inversion). btrfs and xfs store this. */
export const crc32c = (buf) => ~crc32cRaw(0xffffffff, buf) >>> 0;
```

`ts/tests/robustness/corpus/tree.mjs`:

```js
/** The known tree every populated base image holds. */
import { lutimesSync, mkdirSync, rmSync, symlinkSync, utimesSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { rng } from './rng.mjs';

/** 2026-01-01T00:00:00Z: every file's mtime, and the fake mkfs clock. */
export const EPOCH = 1767225600;

/** `docs/` is the directory whose inode the ext4 zero-inode mutations hit. */
export const TREE = [
    { path: 'hello.txt', text: 'hello, anyfs\n' },
    { path: 'docs/readme.md', text: '# anyfs robustness corpus\n'.repeat(80) },
    { path: 'docs/nested/deep/leaf.txt', text: 'leaf\n' },
    { path: 'data/blob.bin', random: 300 * 1024, seed: 1 },
    { path: 'data/small.bin', random: 4096, seed: 2 },
    { path: 'link', symlink: 'hello.txt' },
];

function randomBytes(n, seed) {
    const next = rng(seed);
    const b = Buffer.alloc(n);
    for (let i = 0; i < n; i++) b[i] = Math.floor(next() * 256);
    return b;
}

/** Write TREE under `dir` (wiped first). `symlinks: false` drops the
 *  symlink, for filesystems that cannot store one (FAT). */
export function writeTree(dir, { symlinks = true } = {}) {
    rmSync(dir, { recursive: true, force: true });
    mkdirSync(dir, { recursive: true });
    const paths = new Set();
    for (const e of TREE) {
        if (e.symlink && !symlinks) continue;
        const p = join(dir, e.path);
        mkdirSync(dirname(p), { recursive: true });
        if (e.symlink) symlinkSync(e.symlink, p);
        else if (e.text) writeFileSync(p, e.text);
        else writeFileSync(p, randomBytes(e.random, e.seed));
        for (let q = e.path; q !== '.'; q = dirname(q)) paths.add(q);
    }
    for (const q of paths) lutimesSync(join(dir, q), EPOCH, EPOCH);
    utimesSync(dir, EPOCH, EPOCH);
}
```

`ts/tests/robustness/corpus/tools.mjs`:

```js
/** Running the image tools: mkfs and friends live in /sbin, off a normal PATH. */
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { delimiter, join } from 'node:path';

const PATH = ['/usr/sbin', '/sbin', process.env.PATH ?? ''].join(delimiter);

/** The Debian package that ships each tool, for the error message. */
const PROVIDER = {
    mke2fs: 'e2fsprogs',
    debugfs: 'e2fsprogs',
    'mkfs.fat': 'dosfstools',
    mcopy: 'mtools',
    'mkfs.exfat': 'exfatprogs',
    'mkfs.f2fs': 'f2fs-tools',
    mkntfs: 'ntfs-3g',
    'mkfs.btrfs': 'btrfs-progs',
    'mkfs.xfs': 'xfsprogs',
    xorriso: 'xorriso',
    mksquashfs: 'squashfs-tools',
    'qemu-img': 'qemu-utils',
    sfdisk: 'fdisk',
};

export function which(tool) {
    for (const d of PATH.split(delimiter)) {
        if (d && existsSync(join(d, tool))) return join(d, tool);
    }
    return null;
}

/** Throw naming every missing tool and the apt packages that provide them. */
export function requireTools(tools) {
    const missing = tools.filter((t) => !which(t));
    if (missing.length === 0) return;
    const pkgs = [...new Set(missing.map((t) => PROVIDER[t] ?? t))].join(' ');
    throw new Error(
        `missing tools: ${missing.join(', ')} — install with: sudo apt-get install ${pkgs}`,
    );
}

/** Run a tool; return stdout, or throw with its stderr. */
export function run(tool, args, { env = {}, cwd, input } = {}) {
    const res = spawnSync(which(tool) ?? tool, args, {
        cwd,
        input,
        encoding: 'utf-8',
        env: { ...process.env, PATH, ...env },
    });
    if (res.error) throw res.error;
    if (res.status !== 0) {
        throw new Error(`${tool} ${args.join(' ')} failed (exit ${res.status}):\n${res.stderr}`);
    }
    return res.stdout;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `node --test ts/tests/robustness/test/corpus.test.mjs`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness
git add ts/tests/robustness
git commit -m "test(robustness): corpus scaffolding — paths, PRNG, crc32c, tree, tools

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Base images and layout probes

**Files:**
- Create: `ts/tests/robustness/corpus/layout.mjs`
- Create: `ts/tests/robustness/corpus/bases.mjs`
- Create: `ts/tests/robustness/corpus/mutations.mjs` (checksum fix-ups only, for now)
- Test: `ts/tests/robustness/test/corpus.test.mjs` (append)

- [ ] **Step 1: Write the failing tests**

Append to `ts/tests/robustness/test/corpus.test.mjs`. First, merge these into the import block at the top:

```js
import { before } from 'node:test';
import { join } from 'node:path';
import { BASES, TOOLS, buildAllBases } from '../corpus/bases.mjs';
import { fixBtrfsSbCsum, fixExt4SbCsum, fixGptCrcs, fixXfsSbCrc } from '../corpus/mutations.mjs';
import { requireTools } from '../corpus/tools.mjs';
import { SCRATCH_DIR } from '../lib/paths.mjs';
```

Then append:

```js
// Built once for every test below: needs the image tools (Task 0).
let bases;
before(() => {
    requireTools(TOOLS);
    bases = buildAllBases(join(SCRATCH_DIR, 'bases'));
});

test('there are 16 bases', () => {
    assert.equal(Object.keys(BASES).length, 16);
});

test('checksum fix-ups reproduce the checksums mkfs wrote', () => {
    for (const [name, fix] of [
        ['ext4', fixExt4SbCsum],
        ['btrfs', fixBtrfsSbCsum],
        ['xfs', fixXfsSbCrc],
        ['gpt', fixGptCrcs],
    ]) {
        const b = Buffer.from(bases.bufs[name]);
        fix(b);
        assert.ok(b.equals(bases.bufs[name]), `${name}: recomputed checksum differs`);
    }
});

test('layouts point at the structures they name', () => {
    const { bufs, layouts } = bases;

    const e = layouts.ext4;
    assert.notEqual(e.rootInode, e.docsInode);
    assert.equal(bufs.ext4.readUInt16LE(e.rootInode) & 0xf000, 0x4000, 'root inode is a dir');
    assert.equal(bufs.ext4.readUInt16LE(e.docsInode) & 0xf000, 0x4000, 'docs inode is a dir');
    assert.equal(bufs.ext4panic.readUInt16LE(1024 + 60), 3, 'ext4panic sb says errors=panic');

    const v = layouts.vfat;
    assert.equal(bufs.vfat.readUInt16LE(v.fatOffset), 0xfff8, 'FAT16 media entry');
    const rootDir = bufs.vfat.subarray(v.rootDirOffset, v.rootDirOffset + v.rootDirBytes);
    assert.ok(rootDir.includes('HELLO   TXT'), 'root dir holds the 8.3 entry of hello.txt');

    const bt = layouts.btrfs;
    assert.ok(bt.treeBlocks['3']?.length > 0, 'chunk tree blocks found');
    assert.ok(bt.treeBlocks['5']?.length > 0, 'fs tree blocks found');

    assert.equal(bufs.xfs.readUInt16BE(layouts.xfs.rootInode), 0x494e, 'xfs root inode magic');

    const iso = layouts.iso9660;
    assert.equal(iso.blockSize, 2048);
    assert.ok(bufs.iso9660[iso.rootDirOffset] >= 34, 'root dir starts with a record');
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `node --test ts/tests/robustness/test/corpus.test.mjs`
Expected: FAIL with `Cannot find module '.../corpus/bases.mjs'`.

- [ ] **Step 3: Create `layout.mjs`**

`ts/tests/robustness/corpus/layout.mjs`:

```js
/**
 * Where the structures the mutations target live in each base image.
 * Computed once per base; the mutations themselves are pure functions of
 * (base bytes, layout, seed).
 */
import { run } from './tools.mjs';

const LOCATED = /located at block (\d+), offset (0x[0-9a-f]+)/;

/** Byte offset of an ext4 inode via debugfs (`spec`: `<N>` or a path). */
function ext4Inode(file, spec, blockSize) {
    const out = run('debugfs', ['-R', `imap ${spec}`, file]);
    const m = LOCATED.exec(out);
    if (!m) throw new Error(`debugfs imap ${spec}: unexpected output:\n${out}`);
    return Number(m[1]) * blockSize + Number.parseInt(m[2], 16);
}

export function probeLayout(probe, file, buf) {
    switch (probe) {
        case 'ext4': {
            const sb = buf.subarray(1024, 2048);
            const blockSize = 1024 << sb.readUInt32LE(24);
            return {
                blockSize,
                inodeSize: sb.readUInt16LE(88),
                // The group descriptor table is the block after the superblock's.
                gdtOffset: (sb.readUInt32LE(20) + 1) * blockSize,
                rootInode: ext4Inode(file, '<2>', blockSize),
                docsInode: ext4Inode(file, '/docs', blockSize),
            };
        }
        case 'vfat': {
            const bps = buf.readUInt16LE(11);
            const reserved = buf.readUInt16LE(14);
            const fats = buf[16];
            const fatSectors = buf.readUInt16LE(22);
            return {
                fatOffset: reserved * bps,
                fatBytes: fatSectors * bps,
                rootDirOffset: (reserved + fats * fatSectors) * bps,
                rootDirBytes: buf.readUInt16LE(17) * 32,
            };
        }
        case 'btrfs': {
            const sb = buf.subarray(0x10000, 0x11000);
            const fsid = sb.subarray(0x20, 0x30);
            const sectorSize = sb.readUInt32LE(0x90);
            // Tree block headers carry the fsid at 0x20 and the owning tree at
            // 0x58. DUP metadata means several copies per tree: keep them all.
            const treeBlocks = {};
            for (let off = 0; off + sectorSize <= buf.length; off += sectorSize) {
                if (off === 0x10000) continue; // the superblock carries the fsid too
                if (!buf.subarray(off + 0x20, off + 0x30).equals(fsid)) continue;
                const owner = buf.readBigUInt64LE(off + 0x58);
                if (owner > 255n) continue; // log/reloc trees: not targeted
                (treeBlocks[String(owner)] ??= []).push(off);
            }
            return { nodeSize: sb.readUInt32LE(0x94), sectorSize, treeBlocks };
        }
        case 'xfs': {
            const blockSize = buf.readUInt32BE(4);
            const sectSize = buf.readUInt16BE(102);
            const inodeSize = buf.readUInt16BE(104);
            const agBlocks = buf.readUInt32BE(84);
            const inopblog = BigInt(buf[123]);
            const agblklog = BigInt(buf[124]);
            const ino = buf.readBigUInt64BE(56);
            const agno = Number(ino >> (agblklog + inopblog));
            const agino = ino & ((1n << (agblklog + inopblog)) - 1n);
            const agbno = Number(agino >> inopblog);
            const slot = Number(agino & ((1n << inopblog) - 1n));
            return {
                sectSize,
                inodeSize,
                agfOffset: sectSize, // AGF: sector 1 of AG 0
                agiOffset: 2 * sectSize, // AGI: sector 2 of AG 0
                rootInode: (agno * agBlocks + agbno) * blockSize + slot * inodeSize,
            };
        }
        case 'iso9660': {
            const pvd = 16 * 2048;
            if (buf.toString('latin1', pvd + 1, pvd + 6) !== 'CD001') {
                throw new Error('iso9660: no primary volume descriptor at sector 16');
            }
            const blockSize = buf.readUInt16LE(pvd + 128);
            return {
                pvdOffset: pvd,
                blockSize,
                rootDirOffset: buf.readUInt32LE(pvd + 158) * blockSize,
                pathTableOffset: buf.readUInt32LE(pvd + 140) * blockSize,
                pathTableBytes: buf.readUInt32LE(pvd + 132),
            };
        }
        default:
            throw new Error(`no layout probe for ${probe}`);
    }
}
```

- [ ] **Step 4: Create `bases.mjs`**

`ts/tests/robustness/corpus/bases.mjs`:

```js
/**
 * Base images for the robustness corpus, built rootless from the known
 * tree. Containers and partitioned disks are assembled from the
 * single-filesystem bases listed before them. UUIDs and clocks are pinned
 * where the tool allows it; the rest (btrfs device UUIDs, …) stays random,
 * so cases.json records every image's sha256.
 */
import {
    closeSync,
    mkdirSync,
    openSync,
    readFileSync,
    readdirSync,
    rmSync,
    truncateSync,
    writeFileSync,
    writeSync,
} from 'node:fs';
import { join } from 'node:path';
import { probeLayout } from './layout.mjs';
import { run } from './tools.mjs';
import { EPOCH, TREE, writeTree } from './tree.mjs';

const MiB = 1 << 20;
const UUID = '0a0b0c0d-1111-2222-3333-444455556666';
const E2FS_ENV = { E2FSPROGS_FAKE_TIME: String(EPOCH) };
/** mkfs.xfs refuses filesystems under 300 MB unless it believes fstests runs it. */
const XFS_SMALL_ENV = { TEST_DIR: '1', TEST_DEV: '1', QA_CHECK_FS: '1' };

export const TOOLS = [
    'mke2fs',
    'debugfs',
    'mkfs.fat',
    'mcopy',
    'mkfs.exfat',
    'mkfs.f2fs',
    'mkntfs',
    'mkfs.btrfs',
    'mkfs.xfs',
    'xorriso',
    'mksquashfs',
    'qemu-img',
    'sfdisk',
];

/** A fresh sparse file of `size` bytes. */
function sparse(file, size) {
    rmSync(file, { force: true });
    closeSync(openSync(file, 'w'));
    truncateSync(file, size);
}

/** Copy image `src` into disk `dst` at byte offset `at`. */
function place(dst, src, at) {
    const fd = openSync(dst, 'r+');
    try {
        const b = readFileSync(src);
        writeSync(fd, b, 0, b.length, at);
    } finally {
        closeSync(fd);
    }
}

function mke2fs(file, sizeMiB, type, tree, extra = []) {
    sparse(file, sizeMiB * MiB);
    run(
        'mke2fs',
        [
            '-q',
            '-F',
            '-t',
            type,
            // 1 KiB blocks and 256-byte inodes: root (2) and /docs land in
            // different inode-table blocks.
            '-b',
            '1024',
            '-I',
            '256',
            '-N',
            '128',
            '-U',
            UUID,
            '-E',
            `hash_seed=${UUID}`,
            ...extra,
            '-d',
            tree,
            file,
        ],
        { env: E2FS_ENV },
    );
}

/** mkfs.xfs protofile describing TREE, file sources under `tree`. */
export function xfsProto(tree) {
    const root = new Map();
    for (const e of TREE) {
        const parts = e.path.split('/');
        let m = root;
        for (const d of parts.slice(0, -1)) {
            if (!m.has(d)) m.set(d, new Map());
            m = m.get(d);
        }
        m.set(parts.at(-1), e);
    }
    const lines = ['/dev/null', '0 0', 'd--755 0 0'];
    const emit = (m, rel, depth) => {
        const pad = ' '.repeat(depth);
        for (const [name, v] of [...m].sort(([a], [b]) => a.localeCompare(b))) {
            if (v instanceof Map) {
                lines.push(`${pad}${name} d--755 0 0`);
                emit(v, `${rel}${name}/`, depth + 1);
                lines.push(`${pad}$`);
            } else if (v.symlink) {
                lines.push(`${pad}${name} l--777 0 0 ${v.symlink}`);
            } else {
                lines.push(`${pad}${name} ---644 0 0 ${join(tree, rel, name)}`);
            }
        }
    };
    emit(root, '', 1);
    lines.push('$');
    return `${lines.join('\n')}\n`;
}

const GPT_SCRIPT = `label: gpt
label-id: 0A0B0C0D-1111-2222-3333-444455556666
first-lba: 2048
start=2048, size=32768, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=0A0B0C0D-1111-2222-3333-000000000001
start=34816, size=32768, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=0A0B0C0D-1111-2222-3333-000000000002
`;
const MBR_SCRIPT = `label: dos
label-id: 0x0a0b0c0d
start=2048, size=32768, type=83
start=34816, size=32768, type=c
`;
// p1 ext4, p2 extended, p5 (logical) vfat. The EBR sits at the extended start.
const MBREXT_SCRIPT = `label: dos
label-id: 0x0a0b0c0e
start=2048, size=32768, type=83
start=34816, size=36864, type=5
start=36864, size=32768, type=c
`;

function sfdiskDisk(file, script, parts, ctx) {
    sparse(file, 48 * MiB);
    run('sfdisk', ['-q', '--no-reread', '--no-tell-kernel', file], { input: script });
    for (const [base, sector] of parts) place(file, ctx.built[base], sector * 512);
}

/**
 * name → { fs, ext, probe?, build(file, ctx) }. ctx: { posix, fat, scratch,
 * built } — the two tree dirs, a scratch dir, and paths of bases built so
 * far. Insertion order is build order.
 */
export const BASES = {
    ext4: { fs: 'ext4', ext: 'img', probe: 'ext4', build: (f, c) => mke2fs(f, 16, 'ext4', c.posix) },
    ext4panic: {
        fs: 'ext4',
        ext: 'img',
        probe: 'ext4',
        // The superblock asks for a panic on the first error (acceptance #4).
        build: (f, c) => mke2fs(f, 16, 'ext4', c.posix, ['-e', 'panic']),
    },
    ext2: { fs: 'ext2', ext: 'img', build: (f, c) => mke2fs(f, 8, 'ext2', c.posix) },
    vfat: {
        fs: 'vfat',
        ext: 'img',
        probe: 'vfat',
        build: (f, c) => {
            sparse(f, 16 * MiB);
            run('mkfs.fat', ['-F', '16', '-i', '0a0b0c0d', '--invariant', '-n', 'ANYFS', f]);
            const top = readdirSync(c.fat).map((n) => join(c.fat, n));
            run('mcopy', ['-s', '-m', '-i', f, ...top, '::/'], { env: { MTOOLS_SKIP_CHECK: '1' } });
        },
    },
    exfat: {
        fs: 'exfat',
        ext: 'img',
        build: (f) => {
            sparse(f, 16 * MiB);
            run('mkfs.exfat', ['-L', 'ANYFS', f]);
        },
    },
    f2fs: {
        fs: 'f2fs',
        ext: 'img',
        build: (f) => {
            sparse(f, 64 * MiB);
            run('mkfs.f2fs', ['-q', '-f', '-l', 'ANYFS', f]);
        },
    },
    ntfs: {
        fs: 'ntfs',
        ext: 'img',
        build: (f) => {
            sparse(f, 16 * MiB);
            run('mkntfs', ['-F', '-Q', '-q', '-L', 'ANYFS', '-s', '512', f]);
        },
    },
    btrfs: {
        fs: 'btrfs',
        ext: 'img',
        probe: 'btrfs',
        build: (f, c) => {
            sparse(f, 32 * MiB);
            run('mkfs.btrfs', ['-q', '-f', '--mixed', '-U', UUID, '--rootdir', c.posix, f]);
        },
    },
    xfs: {
        fs: 'xfs',
        ext: 'img',
        probe: 'xfs',
        build: (f, c) => {
            const proto = join(c.scratch, 'xfs.proto');
            writeFileSync(proto, xfsProto(c.posix));
            sparse(f, 32 * MiB);
            run('mkfs.xfs', ['-q', '-f', '-m', `uuid=${UUID}`, '-p', proto, f], {
                env: XFS_SMALL_ENV,
            });
        },
    },
    iso9660: {
        fs: 'iso9660',
        ext: 'iso',
        probe: 'iso9660',
        build: (f, c) => {
            rmSync(f, { force: true });
            // Rock Ridge only: a Joliet SVD would shadow the PVD the mutations edit.
            run('xorriso', ['-as', 'mkisofs', '-quiet', '-R', '-V', 'ANYFS', '-o', f, c.posix], {
                env: { SOURCE_DATE_EPOCH: String(EPOCH) },
            });
        },
    },
    squashfs: {
        fs: 'squashfs',
        ext: 'img',
        build: (f, c) => {
            rmSync(f, { force: true });
            run('mksquashfs', [
                c.posix,
                f,
                '-quiet',
                '-no-progress',
                '-noappend',
                '-all-root',
                '-mkfs-time',
                String(EPOCH),
                '-all-time',
                String(EPOCH),
            ]);
        },
    },
    qcow2: {
        fs: 'ext4 (qcow2)',
        ext: 'qcow2',
        build: (f, c) =>
            run('qemu-img', ['convert', '-q', '-f', 'raw', '-O', 'qcow2', c.built.ext4, f]),
    },
    vmdk: {
        fs: 'ext4 (vmdk)',
        ext: 'vmdk',
        build: (f, c) =>
            run('qemu-img', [
                'convert',
                '-q',
                '-f',
                'raw',
                '-O',
                'vmdk',
                '-o',
                'subformat=monolithicSparse',
                c.built.ext4,
                f,
            ]),
    },
    gpt: {
        fs: 'ext4+vfat (gpt)',
        ext: 'img',
        build: (f, c) =>
            sfdiskDisk(f, GPT_SCRIPT, [
                ['ext4', 2048],
                ['vfat', 34816],
            ], c),
    },
    mbr: {
        fs: 'ext4+vfat (mbr)',
        ext: 'img',
        build: (f, c) =>
            sfdiskDisk(f, MBR_SCRIPT, [
                ['ext4', 2048],
                ['vfat', 34816],
            ], c),
    },
    mbrext: {
        fs: 'ext4+vfat (mbr, extended)',
        ext: 'img',
        build: (f, c) =>
            sfdiskDisk(f, MBREXT_SCRIPT, [
                ['ext4', 2048],
                ['vfat', 36864],
            ], c),
    },
};

/** Build every base under `scratch`. Returns { files, bufs, layouts }. */
export function buildAllBases(scratch, { log = () => {} } = {}) {
    mkdirSync(scratch, { recursive: true });
    const ctx = {
        posix: join(scratch, 'tree-posix'),
        fat: join(scratch, 'tree-fat'),
        scratch,
        built: {},
    };
    writeTree(ctx.posix);
    writeTree(ctx.fat, { symlinks: false });
    const bufs = {};
    const layouts = {};
    for (const [name, b] of Object.entries(BASES)) {
        const file = join(scratch, `${name}.${b.ext}`);
        b.build(file, ctx);
        ctx.built[name] = file;
        bufs[name] = readFileSync(file);
        if (b.probe) layouts[name] = probeLayout(b.probe, file, bufs[name]);
        log(name);
    }
    return { files: ctx.built, bufs, layouts };
}
```

- [ ] **Step 5: Create `mutations.mjs` with the checksum fix-ups**

`ts/tests/robustness/corpus/mutations.mjs`:

```js
/**
 * Deterministic mutations of the base images. Each is a pure function of
 * (base bytes, layout[, seed]) and returns a new buffer; the base is never
 * modified.
 */
import { crc32 } from 'node:zlib';
import { crc32c, crc32cRaw } from './checksum.mjs';

// ── Checksum fix-ups ──
// Re-checksum after editing a field, so the driver parses the extreme value
// itself instead of stopping at the checksum check.

/** ext4: raw crc32c of superblock bytes 0..1019, only with metadata_csum. */
export function fixExt4SbCsum(img) {
    const sb = img.subarray(1024, 2048);
    if (!(sb.readUInt32LE(100) & 0x400)) return;
    sb.writeUInt32LE(crc32cRaw(0xffffffff, sb.subarray(0, 1020)), 1020);
}

/** btrfs: standard crc32c of superblock [0x20, 0x1000), stored at 0. */
export function fixBtrfsSbCsum(img) {
    const sb = img.subarray(0x10000, 0x11000);
    sb.writeUInt32LE(crc32c(sb.subarray(0x20)), 0);
}

/** xfs: standard crc32c of the superblock sector, crc field zeroed. */
export function fixXfsSbCrc(img) {
    const sb = img.subarray(0, img.readUInt16BE(102));
    sb.writeUInt32LE(0, 224);
    sb.writeUInt32LE(crc32c(sb), 224);
}

/** GPT: CRC-32 of the entry array, then of the primary header. */
export function fixGptCrcs(img) {
    const hdr = img.subarray(512, 1024);
    const at = Number(hdr.readBigUInt64LE(72)) * 512;
    const bytes = hdr.readUInt32LE(80) * hdr.readUInt32LE(84);
    hdr.writeUInt32LE(crc32(img.subarray(at, at + bytes)), 88);
    hdr.writeUInt32LE(0, 16);
    hdr.writeUInt32LE(crc32(hdr.subarray(0, hdr.readUInt32LE(12))), 16);
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `node --test ts/tests/robustness/test/corpus.test.mjs`
Expected: PASS (5 tests). If an assertion about a layout fails, fix the probe or the builder, not the
assertion: each one checks a fact about the image format.

- [ ] **Step 7: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness
git add ts/tests/robustness
git commit -m "test(robustness): rootless base images and layout probes

16 bases: ext4 (+ an errors=panic twin), ext2, vfat, exfat, f2fs, ntfs,
btrfs, xfs, iso9660, squashfs, ext4 as qcow2/vmdk, and GPT / MBR /
MBR-extended disks. Layout probes locate the superblocks, inodes, FATs
and tree blocks the mutations target; checksum fix-ups are verified
against mkfs output.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Mutations

**Files:**
- Modify: `ts/tests/robustness/corpus/mutations.mjs` (append)
- Test: `ts/tests/robustness/test/corpus.test.mjs` (append)

- [ ] **Step 1: Write the failing tests**

Merge into the imports of `corpus.test.mjs`:

```js
import { createHash } from 'node:crypto';
import { flip, mutationCases, truncate } from '../corpus/mutations.mjs';
```

Append:

```js
const sha = (b) => createHash('sha256').update(b).digest('hex');

test('62 mutations over the bases, 78 cases in all, unique names', () => {
    const m = mutationCases();
    assert.equal(m.length, 62);
    const names = [...Object.keys(BASES).map((b) => `${b}-base`), ...m.map((x) => x.name)];
    assert.equal(names.length, 78);
    assert.equal(new Set(names).size, names.length);
    for (const x of m) assert.ok(BASES[x.base], `${x.name}: unknown base ${x.base}`);
});

test('mutations are pure, deterministic and actually change the image', () => {
    for (const m of mutationCases()) {
        const base = bases.bufs[m.base];
        const before = sha(base);
        const a = m.apply(base, bases.layouts[m.base]);
        const b = m.apply(base, bases.layouts[m.base]);
        assert.equal(sha(base), before, `${m.name} modified its base`);
        assert.ok(a.equals(b), `${m.name} is not deterministic`);
        assert.ok(!a.equals(base), `${m.name} left the image unchanged`);
    }
});

test('flip and truncate', () => {
    const z = Buffer.alloc(1 << 20);
    const f = flip(z, 1, 1e-3);
    let changed = 0;
    for (const x of f) if (x) changed++;
    assert.ok(changed > 900 && changed <= 1049, `changed ${changed}`);
    assert.equal(truncate(Buffer.alloc(10_000), 25).length, 2048);
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `node --test ts/tests/robustness/test/corpus.test.mjs`
Expected: FAIL. `mutationCases` is not exported.

- [ ] **Step 3: Implement the mutations**

Append to `ts/tests/robustness/corpus/mutations.mjs` (and add `import { randInt, rng } from './rng.mjs';`
to its imports):

```js
const MiB = 1 << 20;

/** Edit a copy, then optionally re-checksum it. */
const edit = (fn, fix) => (buf, layout) => {
    const out = Buffer.from(buf);
    fn(out, layout);
    fix?.(out);
    return out;
};

/** Zero the [offset, length] ranges `pick(layout)` names, in a copy. */
const zero = (pick) => (buf, layout) => {
    const out = Buffer.from(buf);
    for (const [off, len] of pick(layout)) out.fill(0, off, off + len);
    return out;
};

/** Random byte corruption over the first 4 MiB: XOR `density * span` bytes. */
export function flip(buf, seed, density) {
    const out = Buffer.from(buf);
    const span = Math.min(out.length, 4 * MiB);
    const next = rng(seed);
    const n = Math.max(1, Math.round(span * density));
    for (let i = 0; i < n; i++) out[randInt(next, span)] ^= 1 + randInt(next, 255);
    return out;
}

/** The first `pct` % of the image, rounded down to a sector. */
export const truncate = (buf, pct) =>
    Buffer.from(buf.subarray(0, Math.floor((buf.length * pct) / 100 / 512) * 512));

/** 1. Superblock fields set to extreme values; magic kept, checksum fixed. */
const SB_EDITS = {
    ext4: [
        ['sb-blocks-count', (b) => b.writeUInt32LE(0xffffffff, 1024 + 4)],
        ['sb-inodes-per-group', (b) => b.writeUInt32LE(0xffffffff, 1024 + 40)],
        ['sb-log-block-size', (b) => b.writeUInt32LE(31, 1024 + 24)],
    ],
    vfat: [
        ['sb-sectors-per-cluster', (b) => b.writeUInt8(0, 13)],
        ['sb-reserved-sectors', (b) => b.writeUInt16LE(0xffff, 14)],
        [
            'sb-total-sectors',
            (b) => {
                b.writeUInt16LE(0, 19);
                b.writeUInt32LE(0xffffffff, 32);
            },
        ],
    ],
    btrfs: [
        ['sb-nodesize', (b) => b.writeUInt32LE(1 << 20, 0x10000 + 0x94)],
        ['sb-root', (b) => b.writeBigUInt64LE(0x7fffffff0000n, 0x10000 + 0x50)],
    ],
    xfs: [
        ['sb-agcount', (b) => b.writeUInt32BE(0xffffffff, 88)],
        ['sb-rootino', (b) => b.writeBigUInt64BE(0xfffffffff0n, 56)],
    ],
    iso9660: [
        [
            'pvd-block-size',
            (b, l) => {
                b.writeUInt16LE(1, l.pvdOffset + 128);
                b.writeUInt16BE(1, l.pvdOffset + 130);
            },
        ],
        [
            'pvd-root-extent',
            (b, l) => {
                b.writeUInt32LE(0x7fffffff, l.pvdOffset + 158);
                b.writeUInt32BE(0x7fffffff, l.pvdOffset + 162);
            },
        ],
    ],
};
const SB_FIX = { ext4: fixExt4SbCsum, btrfs: fixBtrfsSbCsum, xfs: fixXfsSbCrc };

/** 2. Zeroed metadata. */
const ZEROS = {
    ext4: [
        ['zero-gdt', (l) => [[l.gdtOffset, l.blockSize]]],
        ['zero-root-inode', (l) => [[l.rootInode, l.inodeSize]]],
        ['zero-docs-inode', (l) => [[l.docsInode, l.inodeSize]]],
    ],
    // Mounts, then the first lookup of docs/ hits a bad inode — which this
    // superblock says should panic the kernel.
    ext4panic: [['zero-docs-inode', (l) => [[l.docsInode, l.inodeSize]]]],
    vfat: [
        ['zero-fat', (l) => [[l.fatOffset, l.fatBytes]]],
        ['zero-rootdir', (l) => [[l.rootDirOffset, l.rootDirBytes]]],
    ],
    btrfs: [
        ['zero-chunk-tree', (l) => l.treeBlocks['3'].map((o) => [o, l.nodeSize])],
        ['zero-fs-tree', (l) => l.treeBlocks['5'].map((o) => [o, l.nodeSize])],
    ],
    xfs: [
        ['zero-agf', (l) => [[l.agfOffset, l.sectSize]]],
        ['zero-agi', (l) => [[l.agiOffset, l.sectSize]]],
        ['zero-root-inode', (l) => [[l.rootInode, l.inodeSize]]],
    ],
    iso9660: [
        ['zero-rootdir', (l) => [[l.rootDirOffset, l.blockSize]]],
        ['zero-path-table', (l) => [[l.pathTableOffset, l.pathTableBytes]]],
    ],
};

/** 3 + 4. Byte flips and truncation run over these. */
const FLIP_TRUNC = ['ext4', 'vfat', 'btrfs', 'xfs', 'iso9660'];

const MBR = (i) => 446 + 16 * i;
const gptEntry = (b, i) => Number(b.readBigUInt64LE(512 + 72)) * 512 + i * b.readUInt32LE(512 + 84);

/** 5 + 6. Container headers and partition tables: [name, edit, fix?]. */
const HEADER_EDITS = {
    qcow2: [
        ['l1-offset', (b) => b.writeBigUInt64BE(0x7ffffffffff00000n, 40)],
        ['refcount-offset', (b) => b.writeBigUInt64BE(0x7ffffffffff00000n, 48)],
        ['cluster-bits', (b) => b.writeUInt32BE(31, 20)],
        ['l1-size', (b) => b.writeUInt32BE(0x7fffffff, 36)],
    ],
    vmdk: [
        ['capacity', (b) => b.writeBigUInt64LE(1n << 62n, 12)],
        ['grain-size', (b) => b.writeBigUInt64LE(1n << 40n, 20)],
        ['gd-offset', (b) => b.writeBigUInt64LE(0x00ffffffffffff00n, 56)],
    ],
    mbr: [
        // p2 starts inside p1.
        ['overlap', (b) => b.writeUInt32LE(b.readUInt32LE(MBR(0) + 8) + 1024, MBR(1) + 8)],
        [
            'out-of-range',
            (b) => {
                b.writeUInt32LE(0xffff0000, MBR(1) + 8);
                b.writeUInt32LE(0xffff, MBR(1) + 12);
            },
        ],
    ],
    mbrext: [
        // The first EBR's "next" link points back at that same EBR.
        [
            'ext-loop',
            (b) => {
                const e = b.readUInt32LE(MBR(1) + 8) * 512 + MBR(1);
                b[e + 4] = 0x05;
                b.writeUInt32LE(0, e + 8);
                b.writeUInt32LE(2048, e + 12);
            },
        ],
    ],
    gpt: [
        [
            'overlap',
            (b) => {
                const start = b.readBigUInt64LE(gptEntry(b, 0) + 32);
                b.writeBigUInt64LE(start + 1024n, gptEntry(b, 1) + 32);
            },
            fixGptCrcs,
        ],
        ['out-of-range', (b) => b.writeBigUInt64LE(0xffffffffffffn, gptEntry(b, 1) + 40), fixGptCrcs],
    ],
};

/** Every mutated case: [{ name, base, mutation, apply(buf, layout) }]. */
export function mutationCases() {
    const out = [];
    const add = (base, mutation, apply) =>
        out.push({ name: `${base}-${mutation}`, base, mutation, apply });
    for (const [base, edits] of Object.entries(SB_EDITS)) {
        for (const [m, fn] of edits) add(base, m, edit(fn, SB_FIX[base]));
    }
    for (const [base, zs] of Object.entries(ZEROS)) {
        for (const [m, pick] of zs) add(base, m, zero(pick));
    }
    for (const base of FLIP_TRUNC) {
        add(base, 'flip-s1-d1e-4', (b) => flip(b, 1, 1e-4));
        add(base, 'flip-s2-d1e-3', (b) => flip(b, 2, 1e-3));
        for (const pct of [25, 50, 90]) add(base, `trunc-${pct}`, (b) => truncate(b, pct));
    }
    for (const [base, edits] of Object.entries(HEADER_EDITS)) {
        for (const [m, fn, fix] of edits) add(base, m, edit(fn, fix));
    }
    return out;
}
```

Count check: superblock 12, zeroes 13, flips 10, truncations 15, container 7, partition tables 5,
which makes 62 mutations. With 16 bases that is 78 cases.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `node --test ts/tests/robustness/test/corpus.test.mjs`
Expected: PASS (8 tests).

- [ ] **Step 5: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness
git add ts/tests/robustness
git commit -m "test(robustness): 62 deterministic mutations of the bases

Superblock fields at extreme values (checksums fixed), zeroed metadata,
seeded byte flips, truncation, qcow2/vmdk header fields, and
overlapping / out-of-range / looping partition tables.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: `generate.mjs`

**Files:**
- Create: `ts/tests/robustness/corpus/generate.mjs`

- [ ] **Step 1: Create the generator**

`ts/tests/robustness/corpus/generate.mjs`:

```js
#!/usr/bin/env node
/**
 * Build the robustness corpus: rootless base images of a known tree, plus
 * deterministic mutations of them.
 *   node ts/tests/robustness/corpus/generate.mjs [--force]
 * Writes ~/.cache/anyfs-robustness/generated/<case>.<ext> — read-only, so a
 * run can't modify a case — and cases.json. Does nothing when cases.json and
 * every image already exist, unless --force.
 */
import { createHash } from 'node:crypto';
import {
    chmodSync,
    closeSync,
    existsSync,
    ftruncateSync,
    mkdirSync,
    openSync,
    readFileSync,
    rmSync,
    writeFileSync,
    writeSync,
} from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { CASES_JSON, GENERATED_DIR } from '../lib/paths.mjs';
import { BASES, TOOLS, buildAllBases } from './bases.mjs';
import { mutationCases } from './mutations.mjs';
import { requireTools } from './tools.mjs';

const CHUNK = 64 * 1024;
const ZERO = Buffer.alloc(CHUNK);
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

/** Write `buf` to `file` (read-only), leaving all-zero chunks as holes. */
function writeCase(file, buf) {
    rmSync(file, { force: true });
    const fd = openSync(file, 'w');
    try {
        ftruncateSync(fd, buf.length);
        for (let off = 0; off < buf.length; off += CHUNK) {
            const chunk = buf.subarray(off, Math.min(off + CHUNK, buf.length));
            if (!chunk.equals(ZERO.subarray(0, chunk.length))) {
                writeSync(fd, chunk, 0, chunk.length, off);
            }
        }
    } finally {
        closeSync(fd);
    }
    chmodSync(file, 0o444);
}

function upToDate() {
    if (!existsSync(CASES_JSON)) return false;
    const { cases } = JSON.parse(readFileSync(CASES_JSON, 'utf-8'));
    return cases.every((c) => existsSync(c.file));
}

function main() {
    const { values } = parseArgs({ options: { force: { type: 'boolean', default: false } } });
    if (!values.force && upToDate()) {
        console.log(`corpus up to date: ${CASES_JSON}`);
        return;
    }
    requireTools(TOOLS);
    mkdirSync(GENERATED_DIR, { recursive: true });
    const scratch = join(GENERATED_DIR, '.scratch');
    rmSync(scratch, { recursive: true, force: true });
    const { bufs, layouts } = buildAllBases(scratch, { log: (n) => console.log(`base  ${n}`) });

    const cases = [];
    const emit = (name, base, mutation, buf) => {
        const b = BASES[base];
        const file = join(GENERATED_DIR, `${name}.${b.ext}`);
        writeCase(file, buf);
        cases.push({ name, source: 'generated', base, fs: b.fs, mutation, file, sha256: sha256(buf) });
    };
    for (const base of Object.keys(BASES)) emit(`${base}-base`, base, 'none', bufs[base]);
    for (const m of mutationCases()) {
        emit(m.name, m.base, m.mutation, m.apply(bufs[m.base], layouts[m.base]));
        console.log(`case  ${m.name}`);
    }
    rmSync(scratch, { recursive: true, force: true });
    writeFileSync(
        CASES_JSON,
        `${JSON.stringify({ generatedAt: new Date().toISOString(), cases }, null, 4)}\n`,
    );
    console.log(`${cases.length} cases → ${CASES_JSON}`);
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) main();
```

- [ ] **Step 2: Generate the corpus**

Run: `node ts/tests/robustness/corpus/generate.mjs`
Expected: 16 `base` lines, 62 `case` lines, then `78 cases → …/cases.json`.

Run: `ls -la ~/.cache/anyfs-robustness/generated | head; du -sh ~/.cache/anyfs-robustness/generated`
Expected: files are `-r--r--r--`, and the directory is a few hundred MB at most (holes are sparse).

Run it again: `node ts/tests/robustness/corpus/generate.mjs`
Expected: `corpus up to date: …`.

- [ ] **Step 3: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness/corpus/generate.mjs
git add ts/tests/robustness/corpus/generate.mjs
git commit -m "test(robustness): corpus generator CLI (78 cases, sparse, read-only)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: The syzbot set

**Files:**
- Create: `ts/tests/robustness/lib/glob.mjs`
- Create: `ts/tests/robustness/tools/list-syzbot-candidates.mjs`
- Create: `ts/tests/robustness/fetch-syzbot.mjs`
- Create: `ts/tests/robustness/syzbot.json`

- [ ] **Step 1: Create the `--only` matcher**

`ts/tests/robustness/lib/glob.mjs`:

```js
/** `--only` patterns: comma-separated globs (`*`, `?`) over case names. */
export function matcher(patterns) {
    const res = patterns
        .split(',')
        .map((p) => p.trim())
        .filter(Boolean)
        .map(
            (p) =>
                new RegExp(
                    `^${p
                        .replace(/[.+^${}()|[\]\\]/g, '\\$&')
                        .replace(/\*/g, '.*')
                        .replace(/\?/g, '.')}$`,
                ),
        );
    return (name) => res.some((re) => re.test(name));
}
```

- [ ] **Step 2: Create the curation helper**

`ts/tests/robustness/tools/list-syzbot-candidates.mjs`:

```js
#!/usr/bin/env node
/**
 * One-off curation helper for syzbot.json. Lists syzbot bugs on filesystems
 * anyfs supports whose title points at a read path (mount, lookup, readdir,
 * read) and that ship a "mounted in repro" image. Prints TSV:
 *   subsystem  list  extid  sb_errors  asset  title
 * sb_errors is the ext4 superblock's errors behaviour read from the image
 * (3 = panic); "-" for other filesystems. One request per second.
 *   node ts/tests/robustness/tools/list-syzbot-candidates.mjs [subsystem...]
 */
import { gunzipSync } from 'node:zlib';

const DASH = 'https://syzkaller.appspot.com';
const SUBSYSTEMS = ['ext4', 'btrfs', 'xfs', 'fat', 'hfs', 'isofs', 'udf', 'ntfs3', 'f2fs', 'squashfs', 'exfat'];
const READ_PATH =
    /mount|fill_super|lookup|readdir|iterate|iget|find_entry|search_dir|get_block|map_blocks|bmap|read|getattr|statfs|get_link|listxattr|xattr_list/i;
const WRITE_PATH =
    /write|setattr|truncate|rename|unlink|fallocate|dirty|create|mkdir|evict|sync|discard|remount|resize|balance|quota|commit|punch|orphan|delete|free_blocks|alloc/i;
const PER_LIST = 12;
const ASSET = /https:\/\/storage\.googleapis\.com\/syzbot-assets\/[0-9a-f]+\/mount_\d+\.gz/;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function get(url) {
    await sleep(1000);
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${url} → HTTP ${res.status}`);
    return res;
}

function bugs(html) {
    const out = [];
    const re = /href="\/bug\?extid=([0-9a-f]+)">([^<]+)</g;
    for (let m = re.exec(html); m; m = re.exec(html)) {
        out.push({ extid: m[1], title: m[2].replace(/&#39;/g, "'").replace(/&amp;/g, '&') });
    }
    return out;
}

const subsystems = process.argv.length > 2 ? process.argv.slice(2) : SUBSYSTEMS;
console.log(['subsystem', 'list', 'extid', 'sb_errors', 'asset', 'title'].join('\t'));
for (const s of subsystems) {
    for (const [list, url] of [
        ['open', `${DASH}/upstream/s/${s}`],
        ['fixed', `${DASH}/upstream/fixed?label=subsystems:${s}`],
    ]) {
        let html;
        try {
            html = await (await get(url)).text();
        } catch (e) {
            console.error(`# ${s} ${list}: ${e.message}`);
            continue;
        }
        const picks = bugs(html)
            .filter((b) => READ_PATH.test(b.title) && !WRITE_PATH.test(b.title))
            .slice(0, PER_LIST);
        for (const b of picks) {
            const asset = ASSET.exec(await (await get(`${DASH}/bug?extid=${b.extid}`)).text())?.[0];
            if (!asset) continue;
            let sbErrors = '-';
            if (s === 'ext4') {
                const img = gunzipSync(Buffer.from(await (await get(asset)).arrayBuffer()));
                if (img.length >= 2048 && img.readUInt16LE(1024 + 56) === 0xef53) {
                    sbErrors = String(img.readUInt16LE(1024 + 60));
                }
            }
            console.log([s, list, b.extid, sbErrors, asset, b.title].join('\t'));
        }
    }
}
```

- [ ] **Step 3: Create the fetcher**

`ts/tests/robustness/fetch-syzbot.mjs`:

```js
#!/usr/bin/env node
/**
 * Fetch the curated syzbot images (syzbot.json) into
 * ~/.cache/anyfs-robustness/syzbot/, check each download against its pinned
 * sha256, and unpack it read-only. A missing asset or a hash mismatch is an
 * error, never a silent skip. The images are syzbot's: downloaded at run
 * time, never committed or redistributed.
 *   node ts/tests/robustness/fetch-syzbot.mjs [--only <glob>] [--pin]
 * Prints "<case>\t<image path>" per case. --pin (curation only) records the
 * sha256 of entries that have none.
 */
import { createHash } from 'node:crypto';
import { chmodSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { gunzipSync } from 'node:zlib';
import { matcher } from './lib/glob.mjs';
import { SYZBOT_DIR, SYZBOT_MANIFEST } from './lib/paths.mjs';

export const caseName = (e) => `syz-${e.fs}-${e.extid.slice(0, 8)}`;
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

/** Fetch (if needed) the entries `only` selects; return them as cases. */
export async function fetchSyzbot({ only = null, pin = false } = {}) {
    const manifest = JSON.parse(readFileSync(SYZBOT_MANIFEST, 'utf-8'));
    mkdirSync(SYZBOT_DIR, { recursive: true });
    const cases = [];
    let pinned = false;
    for (const e of manifest) {
        const name = caseName(e);
        if (only && !only(name)) continue;
        if (!e.sha256 && !pin) {
            throw new Error(`${name}: no sha256 pinned in syzbot.json (curation: run with --pin)`);
        }
        const img = join(SYZBOT_DIR, `${e.extid}.img`);
        if (!existsSync(img)) {
            const res = await fetch(e.url);
            if (!res.ok) throw new Error(`${name}: ${e.url} → HTTP ${res.status}`);
            const gz = Buffer.from(await res.arrayBuffer());
            const got = sha256(gz);
            if (!e.sha256) {
                e.sha256 = got;
                pinned = true;
            } else if (got !== e.sha256) {
                throw new Error(`${name}: sha256 mismatch for ${e.url}: got ${got}, pinned ${e.sha256}`);
            }
            const tmp = `${img}.tmp`;
            writeFileSync(tmp, gunzipSync(gz));
            chmodSync(tmp, 0o444);
            renameSync(tmp, img);
        }
        cases.push({
            name,
            source: 'syzbot',
            fs: e.fs,
            mutation: 'syzbot',
            file: img,
            sha256: e.sha256,
            extid: e.extid,
            title: e.title,
            link: e.link,
        });
    }
    if (pinned) writeFileSync(SYZBOT_MANIFEST, `${JSON.stringify(manifest, null, 4)}\n`);
    return cases;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
    const { values } = parseArgs({
        options: { only: { type: 'string' }, pin: { type: 'boolean', default: false } },
    });
    const cases = await fetchSyzbot({
        only: values.only ? matcher(values.only) : null,
        pin: values.pin,
    });
    for (const c of cases) console.log(`${c.name}\t${c.file}`);
}
```

- [ ] **Step 4: List candidates**

Run: `node ts/tests/robustness/tools/list-syzbot-candidates.mjs > ~/.cache/anyfs-robustness/syzbot-candidates.tsv`
Expected: a TSV with a few dozen rows. It takes several minutes because of the one-request-per-second
throttle. `# … HTTP 404` lines on stderr for a subsystem name syzbot doesn't use are fine.

- [ ] **Step 5: Curate `syzbot.json`**

Pick about 20 entries from the TSV:
- 2–3 per filesystem, covering ext4, btrfs, xfs, fat, hfsplus, isofs, udf, ntfs3, f2fs, squashfs and
  exfat where candidates exist;
- a mix of `open` and `fixed`;
- **at least one ext4 entry with `sb_errors` = 3** (acceptance criterion 4). If none of the ext4
  candidates has it, run the helper with just `ext4` and `PER_LIST` raised to 40, and look again;
- skip titles about write paths that slipped through the filter (anyfs mounts read-only).

Write them to `ts/tests/robustness/syzbot.json` as an array. Each entry has the shape below. `fs` is
anyfs's name for the filesystem: `ext4`, `btrfs`, `xfs`, `vfat` (for `fat`), `hfsplus`, `iso9660`
(for `isofs`), `udf`, `ntfs` (for `ntfs3`), `f2fs`, `squashfs`, `exfat`.

```json
{
    "extid": "<extid from the TSV>",
    "title": "<title from the TSV>",
    "link": "https://syzkaller.appspot.com/bug?extid=<extid>",
    "fs": "<anyfs fs name>",
    "url": "<asset from the TSV>",
    "sha256": "",
    "repro_mount_opts": "<option list of the syz_mount_image call in the bug's syz reproducer, informational>",
    "notes": "<open|fixed>; <why it was picked, e.g. 'sb errors=panic'>"
}
```

`repro_mount_opts` comes from the bug page's "syz repro" link (`/text?tag=ReproSyz&x=…`): copy the
option list of the `syz_mount_image$<fs>(…)` call. If there is no syz repro, use `""`. anyfs ignores
this field because it mounts with its own options; it documents what syzbot mounted with.

- [ ] **Step 6: Pin and fetch**

Run: `node ts/tests/robustness/fetch-syzbot.mjs --pin`
Expected: one `syz-<fs>-<extid8>\t…/syzbot/<extid>.img` line per entry; `syzbot.json` now has every
`sha256` filled in.

Run: `node ts/tests/robustness/fetch-syzbot.mjs`
Expected: the same lines, nothing downloaded again.

Run: `node -e 'const m=require("./ts/tests/robustness/syzbot.json"); if(m.some(e=>!e.sha256)) process.exit(1); console.log(m.length, "entries pinned")'`
Expected: `20 entries pinned` (or however many you picked, 18–24).

- [ ] **Step 7: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness
git add ts/tests/robustness/lib/glob.mjs ts/tests/robustness/tools ts/tests/robustness/fetch-syzbot.mjs ts/tests/robustness/syzbot.json
git commit -m "test(robustness): curated syzbot image set, fetched and sha256-pinned

Only the manifest is committed; images are downloaded at run time.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Harness pieces: classify, walk, report, native bridge

**Files:**
- Create: `ts/tests/robustness/lib/classify.mjs`
- Create: `ts/tests/robustness/lib/walk.mjs`
- Create: `ts/tests/robustness/lib/report.mjs`
- Create: `ts/tests/robustness/lib/native-bridge.mjs`
- Test: `ts/tests/robustness/test/harness.test.mjs`

- [ ] **Step 1: Write the failing tests**

Create `ts/tests/robustness/test/harness.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { finalClass, gate, harnessFailures, selfClass } from '../lib/classify.mjs';
import { matcher } from '../lib/glob.mjs';
import { diffRuns } from '../lib/report.mjs';
import { walkAndRead } from '../lib/walk.mjs';

const rec = (name, cls, extra = {}) => ({
    name,
    class: cls,
    mutation: 'x',
    lastStep: 'walk:0',
    reason: 'r',
    sha256: 'h',
    ...extra,
});

test('classes', () => {
    assert.equal(selfClass({ fatal: null, errors: [] }), 'ok');
    assert.equal(selfClass({ fatal: null, errors: [{}] }), 'error');
    assert.equal(selfClass({ fatal: new Error('x'), errors: [{}] }), 'fatal');
    assert.equal(finalClass({ outcome: { class: 'error' }, timedOut: false }), 'error');
    assert.equal(finalClass({ outcome: null, timedOut: true }), 'hang');
    assert.equal(finalClass({ outcome: null, timedOut: false }), 'crash');
});

test('gate rules', () => {
    assert.equal(gate([rec('a', 'ok'), rec('b', 'error'), rec('c', 'fatal')], 'wasm').pass, true);
    assert.equal(gate([rec('a', 'hang')], 'wasm').pass, false);
    assert.equal(gate([rec('a', 'crash')], 'wasm').pass, false);
    assert.equal(gate([rec('a', 'fatal', { reason: null })], 'wasm').pass, false);
    assert.equal(gate([rec('a-base', 'error', { mutation: 'none' })], 'wasm').pass, false);
    const native = gate([rec('a', 'crash')], 'native');
    assert.equal(native.pass, true); // findings, not failures
    assert.equal(native.problems.length, 1);
});

test('harness failures are cases that died before the image mattered', () => {
    const r = harnessFailures([
        rec('a', 'error', { lastStep: 'boot' }),
        rec('b', 'crash', { lastStep: 'spawn' }),
        rec('c', 'error', { lastStep: 'enter:0' }),
        rec('d', 'ok', { lastStep: 'close' }),
    ]);
    assert.deepEqual(
        r.map((x) => x.name),
        ['a', 'b'],
    );
});

test('--only globs', () => {
    const m = matcher('ext4-*, syz-*');
    assert.ok(m('ext4-base'));
    assert.ok(m('syz-xfs-12345678'));
    assert.ok(!m('ext4panic-base'));
    assert.ok(matcher('vfat-sb-?eserved-sectors')('vfat-sb-reserved-sectors'));
});

test('class flips between runs of the same image are reported', () => {
    const prev = { records: [rec('a', 'ok'), rec('b', 'error'), rec('c', 'ok', { sha256: 'old' })] };
    const cur = [rec('a', 'fatal'), rec('b', 'error'), rec('c', 'error')];
    assert.deepEqual(diffRuns(prev, cur), [{ name: 'a', was: 'ok', now: 'fatal' }]);
    assert.deepEqual(diffRuns(null, cur), []);
});

function fakeTree() {
    const dirs = {
        '/m': [
            { name: '.', kind: 'dir' },
            { name: '..', kind: 'dir' },
            { name: 'a.txt', kind: 'file' },
            { name: 'bad', kind: 'dir' },
            { name: 'sub', kind: 'dir' },
            { name: 'l', kind: 'link' },
        ],
        '/m/sub': [{ name: 'b.bin', kind: 'file' }],
    };
    const kinds = { bad: 'dir', sub: 'dir', l: 'link' };
    const calls = [];
    return {
        calls,
        async readdir(p) {
            calls.push(['readdir', p]);
            if (p === '/m/bad') throw new Error('EUCLEAN');
            return dirs[p] ?? [];
        },
        async stat(p) {
            calls.push(['stat', p]);
            return { kind: kinds[p.split('/').pop()] ?? 'file' };
        },
        async readlink(p) {
            calls.push(['readlink', p]);
            return 'a.txt';
        },
        async openFd(p) {
            calls.push(['open', p]);
            return 3;
        },
        async readFd(fd, off, n) {
            calls.push(['read', fd, off, n]);
            return new Uint8Array(10);
        },
        async closeFd(fd) {
            calls.push(['close', fd]);
        },
    };
}

test('walk records failing ops and keeps going', async () => {
    const s = fakeTree();
    const r = await walkAndRead(s, '/m');
    assert.equal(r.entries, 5); // a.txt bad sub l b.bin
    assert.equal(r.files, 2);
    assert.equal(r.bytes, 20);
    assert.deepEqual(r.errors, [{ op: 'readdir', path: '/m/bad', message: 'EUCLEAN' }]);
    assert.ok(s.calls.some(([op, p]) => op === 'readlink' && p === '/m/l'));
    assert.ok(s.calls.some(([op, , , n]) => op === 'read' && n === 64 * 1024));
});

test('walk honours its limits and stops when told', async () => {
    const limits = { entries: 2, depth: 6, files: 20, readBytes: 1 };
    assert.equal((await walkAndRead(fakeTree(), '/m', { limits })).entries, 2);
    const shallow = await walkAndRead(fakeTree(), '/m', { limits: { ...limits, entries: 500, depth: 1 } });
    assert.equal(shallow.entries, 4); // sub/ is not descended
    const s = fakeTree();
    const stopped = await walkAndRead(s, '/m', { stopped: () => true });
    assert.equal(stopped.entries, 0);
    assert.deepEqual(s.calls, []);
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `node --test ts/tests/robustness/test/harness.test.mjs`
Expected: FAIL with `Cannot find module '.../lib/classify.mjs'`.

- [ ] **Step 3: Implement**

`ts/tests/robustness/lib/classify.mjs`:

```js
/** Outcome classes, in report order. */
export const CLASSES = ['ok', 'error', 'fatal', 'hang', 'crash'];

/** What a child that finished its steps reports: fatal wins over errors. */
export function selfClass({ fatal, errors }) {
    if (fatal) return 'fatal';
    return errors.length > 0 ? 'error' : 'ok';
}

/** The parent's verdict on one child: its own outcome if it sent one,
 *  otherwise hang (the outer timeout fired) or crash (it died). */
export function finalClass({ outcome, timedOut }) {
    if (outcome) return outcome.class;
    return timedOut ? 'hang' : 'crash';
}

/** Cases that failed before the image mattered: the harness is broken. */
export function harnessFailures(records) {
    return records.filter(
        (r) => r.class !== 'ok' && ['spawn', 'start', 'boot'].includes(r.lastStep),
    );
}

/**
 * Gate rules: every unmutated base is ok, every fatal carries a reason, and
 * nothing hangs or crashes. On native these are findings: they are listed,
 * but only a wasm run fails.
 */
export function gate(records, backend) {
    const problems = [];
    for (const r of records) {
        if (r.mutation === 'none' && r.class !== 'ok') {
            problems.push(`${r.name}: unmutated base must be ok, got ${r.class} (${r.reason ?? '-'})`);
        }
        if (r.class === 'fatal' && !r.reason) problems.push(`${r.name}: fatal without a reason`);
        if (r.class === 'hang' || r.class === 'crash') {
            problems.push(`${r.name}: ${r.class} at ${r.lastStep} (${r.reason})`);
        }
    }
    return { pass: backend !== 'wasm' || problems.length === 0, problems };
}
```

`ts/tests/robustness/lib/walk.mjs`:

```js
/** Per partition: at most 500 entries and 6 levels, then 64 KiB from each of
 *  at most 20 files. */
export const LIMITS = { entries: 500, depth: 6, files: 20, readBytes: 64 * 1024 };

/**
 * Walk `root` breadth-first within `limits`, then read the files found.
 * An op that rejects is recorded and the walk goes on: one bad inode must
 * not hide the rest of the tree. Stops once `stopped()` is true (the
 * session went fatal). Returns { entries, files, bytes, errors }.
 */
export async function walkAndRead(
    session,
    root,
    { stopped = () => false, onStep = () => {}, limits = LIMITS } = {},
) {
    const errors = [];
    const attempt = async (op, path, fn) => {
        try {
            return await fn();
        } catch (e) {
            errors.push({ op, path, message: e instanceof Error ? e.message : String(e) });
            return undefined;
        }
    };

    onStep('walk');
    const files = [];
    let entries = 0;
    const queue = [{ path: root, depth: 0 }];
    while (queue.length > 0 && entries < limits.entries && !stopped()) {
        const { path, depth } = queue.shift();
        const list = await attempt('readdir', path, () => session.readdir(path));
        if (!list) continue;
        for (const e of list) {
            if (e.name === '.' || e.name === '..') continue;
            if (entries >= limits.entries || stopped()) break;
            entries++;
            const child = path.endsWith('/') ? `${path}${e.name}` : `${path}/${e.name}`;
            const st = await attempt('stat', child, () => session.stat(child));
            const kind = st?.kind ?? e.kind;
            if (kind === 'link') await attempt('readlink', child, () => session.readlink(child));
            else if (kind === 'dir' && depth + 1 < limits.depth) queue.push({ path: child, depth: depth + 1 });
            else if (kind === 'file' && files.length < limits.files) files.push(child);
        }
    }

    onStep('read');
    let bytes = 0;
    for (const f of files) {
        if (stopped()) break;
        const fd = await attempt('open', f, () => session.openFd(f));
        if (fd === undefined) continue;
        const data = await attempt('read', f, () => session.readFd(fd, 0, limits.readBytes));
        if (data) bytes += data.length;
        await attempt('close', f, () => session.closeFd(fd));
    }
    return { entries, files: files.length, bytes, errors };
}
```

`ts/tests/robustness/lib/report.mjs`:

```js
import { CLASSES } from './classify.mjs';

export function counts(records) {
    const c = Object.fromEntries(CLASSES.map((k) => [k, 0]));
    for (const r of records) c[r.class]++;
    return c;
}

/** Cases whose class changed since `prev` on the same image bytes — a
 *  finding in itself, since the corpus is deterministic. */
export function diffRuns(prev, records) {
    if (!prev) return [];
    const was = new Map(prev.records.map((r) => [r.name, r]));
    return records
        .filter((r) => {
            const p = was.get(r.name);
            return p && p.sha256 === r.sha256 && p.class !== r.class;
        })
        .map((r) => ({ name: r.name, was: was.get(r.name).class, now: r.class }));
}

export function formatSummary({ backend, records, verdict, flips, partial }) {
    const c = counts(records);
    const lines = [
        '',
        `=== robustness: ${backend}, ${records.length} cases${partial ? ' (PARTIAL RUN via --only: not a gate result)' : ''} ===`,
        CLASSES.map((k) => `${k} ${c[k]}`).join('   '),
    ];
    const notable = records.filter((r) => ['fatal', 'hang', 'crash'].includes(r.class));
    if (notable.length > 0) {
        lines.push('', 'class  case                                last step     reason');
        for (const r of notable) {
            lines.push(
                `${r.class.padEnd(6)} ${r.name.padEnd(35)} ${String(r.lastStep).padEnd(13)} ${r.reason ?? ''}`,
            );
        }
    }
    if (flips.length > 0) {
        lines.push('', 'class changed since the previous run (a finding):');
        for (const f of flips) lines.push(`  ${f.name}: ${f.was} → ${f.now}`);
    }
    if (verdict.problems.length > 0) {
        lines.push(
            '',
            backend === 'wasm'
                ? 'GATE FAILED:'
                : 'findings (non-gating on native — record them in ts/tests/robustness/FINDINGS.md):',
        );
        for (const p of verdict.problems) lines.push(`  ${p}`);
    } else {
        lines.push('', backend === 'wasm' ? 'gate passed' : 'no native hangs, crashes or base failures');
    }
    return lines.join('\n');
}
```

`ts/tests/robustness/lib/native-bridge.mjs`:

```js
import { createRequire } from 'node:module';
import { NATIVE_ADDON } from './paths.mjs';

/**
 * The addon as an AnyfsNativeBridge (the shape the Electron preload hands
 * NativeSession), so the native backend runs through the same session code
 * — op watchdog included — as the app. Mirrors the IPC handlers in
 * examples/electron-demo/src/main.ts.
 */
export function nativeBridge() {
    const addon = createRequire(import.meta.url)(NATIVE_ADDON);
    const unsupported = async () => {
        throw new Error('not supported in the robustness harness');
    };
    return {
        available: async () => true,
        init: (memMb, loglevel) => addon.kernelInit(memMb >>> 0, loglevel >>> 0),
        diskOpen: (path, flags) => addon.sessionOpen(path, flags >>> 0),
        diskClose: (h) => addon.sessionClose(h),
        diskListJson: (h) => addon.sessionListJson(h),
        diskMetaJson: (h) => addon.sessionMetaJson(h),
        diskEnter: (h, part, flags) => addon.sessionEnter(h, part >>> 0, flags >>> 0),
        readdirJson: (p) => addon.readdirJson(p),
        lstatJson: (p) => addon.lstatJson(p),
        statJson: (p) => addon.statJson(p),
        realpath: (p) => addon.realpath(p),
        readlink: (p) => addon.readlink(p),
        startProxy: unsupported,
        stopProxy: unsupported,
        fileOpen: (p, flags) => addon.fileOpen(p, flags >>> 0),
        pread: (fd, n, off) => addon.pread(fd, n, off),
        fileClose: (fd) => addon.fileClose(fd),
        onFatal: (cb) => {
            addon.onFatal(cb);
            return () => {};
        },
    };
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `node --test ts/tests/robustness/test/harness.test.mjs`
Expected: PASS (7 tests).

- [ ] **Step 5: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness
git add ts/tests/robustness/lib ts/tests/robustness/test/harness.test.mjs
git commit -m "test(robustness): outcome classes, gate rules, bounded walk, report

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: `case-runner.mjs`

**Files:**
- Create: `ts/tests/robustness/case-runner.mjs`

- [ ] **Step 1: Create the runner**

`ts/tests/robustness/case-runner.mjs`:

```js
#!/usr/bin/env node
/**
 * Runs one robustness case in a fresh process (run.mjs forks one per case):
 *   node case-runner.mjs --backend wasm|native --image <path>
 * Steps: boot, open, listParts, then per partition enter / walk / read, then
 * close. Sends {type:'step', step} as it goes and ends with exactly one
 * {type:'outcome', class, lastStep, reason, errors, stats}; run.mjs turns a
 * missing outcome into hang (timed out) or crash (died). Without an IPC
 * channel the messages go to stdout as JSON lines, for debugging one case.
 */
import { basename, dirname } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { NativeSession, NodeWasmSession } from '../../packages/core/dist/index.js';
import { bootNodeKernel } from '../../packages/core/dist/node.js';
import { selfClass } from './lib/classify.mjs';
import { nativeBridge } from './lib/native-bridge.mjs';
import { WASM_NODE_BUNDLE } from './lib/paths.mjs';
import { walkAndRead } from './lib/walk.mjs';

/** Per-op watchdog in the harness; the product default is 60 s. */
const OP_TIMEOUT_MS = 20_000;
/** The provider's attach bound (DEFAULT_ATTACH_TIMEOUT_MS in provider.tsx). */
const ATTACH_TIMEOUT_MS = 120_000;
/** Booting never reads the image; past this the harness itself is broken. */
const BOOT_TIMEOUT_MS = 60_000;
const ANYFS_MOUNT_RDONLY = 1;
/** Container slots are not filesystems; DiskView doesn't offer them either. */
const CONTAINER_KINDS = new Set(['NESTED', 'LVM_PV', 'LUKS']);

const { values: args } = parseArgs({
    options: { backend: { type: 'string' }, image: { type: 'string' } },
});
if (!['wasm', 'native'].includes(args.backend) || !args.image) {
    console.error('usage: case-runner.mjs --backend wasm|native --image <path>');
    process.exit(2);
}

const errors = [];
const stats = { parts: 0, entries: 0, files: 0, bytes: 0 };
let lastStep = 'start';
let fatal = null;
let reported = false;

const message = (e) => (e instanceof Error ? e.message : String(e));
const emit = (m) =>
    process.send
        ? new Promise((resolve) => process.send(m, () => resolve()))
        : Promise.resolve(console.log(JSON.stringify(m)));

function step(name) {
    lastStep = name;
    void emit({ type: 'step', step: name });
}

async function report(cls, reason) {
    if (reported) return;
    reported = true;
    await emit({ type: 'outcome', class: cls, lastStep, reason, errors: errors.slice(0, 20), stats });
    process.exit(0);
}

function onFatal(err) {
    if (fatal) return;
    fatal = err;
    void report('fatal', message(err));
}

if (args.backend === 'wasm') {
    // Mirror the browser worker: there an uncaught error or rejection on the
    // module-owning thread becomes a session fatal (worker.ts posts
    // host-error / host-rejection, WasmSession fires onFatal). Node has no
    // worker around the module, and emscripten rethrows a pthread's abort on
    // this thread right after onAbort.
    process.on('uncaughtException', (e) => onFatal(new Error(`host-error: ${message(e)}`)));
    process.on('unhandledRejection', (e) => onFatal(new Error(`host-rejection: ${message(e)}`)));
}

/** Reject after `ms` with an error marked timedOut. */
function within(ms, what, p) {
    let timer;
    const t = new Promise((_, reject) => {
        timer = setTimeout(
            () =>
                reject(Object.assign(new Error(`${what} timed out after ${ms / 1000}s`), { timedOut: true })),
            ms,
        );
    });
    return Promise.race([p, t]).finally(() => clearTimeout(timer));
}

async function open() {
    step('boot');
    let session;
    if (args.backend === 'wasm') {
        const { default: factory } = await import(pathToFileURL(WASM_NODE_BUNDLE).href);
        const M = await within(BOOT_TIMEOUT_MS, 'boot', bootNodeKernel(dirname(args.image), factory));
        session = new NodeWasmSession(M, { opTimeoutMs: OP_TIMEOUT_MS, readOnly: true });
        session.onFatal(onFatal);
        step('open');
        await within(ATTACH_TIMEOUT_MS, 'attach', session.attachPath(`/work/${basename(args.image)}`));
    } else {
        session = new NativeSession(nativeBridge(), { opTimeoutMs: OP_TIMEOUT_MS });
        session.onFatal(onFatal);
        await within(BOOT_TIMEOUT_MS, 'boot', session.boot(256, 0));
        step('open');
        await within(ATTACH_TIMEOUT_MS, 'attach', session.attachPath(args.image));
    }
    return session;
}

async function main() {
    const session = await open();
    step('listParts');
    const parts = await session.listParts();
    const targets = parts.filter((p) => !CONTAINER_KINDS.has(p.kind)).map((p) => p.index);
    // No partition table: enter the whole disk, as DiskView's #0 does.
    if (targets.length === 0) targets.push(0);
    for (const idx of targets) {
        if (fatal) return;
        step(`enter:${idx}`);
        let mountPoint;
        try {
            mountPoint = await session.enter(idx, ANYFS_MOUNT_RDONLY);
        } catch (e) {
            errors.push({ op: 'enter', path: `#${idx}`, message: message(e) });
            continue;
        }
        stats.parts++;
        const r = await walkAndRead(session, mountPoint, {
            stopped: () => fatal !== null,
            onStep: (s) => step(`${s}:${idx}`),
        });
        stats.entries += r.entries;
        stats.files += r.files;
        stats.bytes += r.bytes;
        errors.push(...r.errors);
    }
    if (fatal) return;
    step('close');
    await session.close();
}

main().then(
    () => {
        if (fatal) return;
        const first = errors[0];
        void report(
            selfClass({ fatal, errors }),
            first ? `${first.op} ${first.path}: ${first.message}` : null,
        );
    },
    (e) => {
        if (fatal) return;
        // Attach past the provider's bound: the app tears the worker down and
        // shows an error. The session is gone, so this counts as a fatal.
        if (e?.timedOut && lastStep === 'open') {
            onFatal(e);
            return;
        }
        errors.push({ op: lastStep, path: args.image, message: message(e) });
        void report('error', `${lastStep}: ${message(e)}`);
    },
);
```

- [ ] **Step 2: Smoke an unmutated base on both backends**

Run:
```bash
node ts/tests/robustness/case-runner.mjs --backend wasm --image ~/.cache/anyfs-robustness/generated/ext4-base.img 2>/dev/null | grep '"outcome"'
node ts/tests/robustness/case-runner.mjs --backend native --image ~/.cache/anyfs-robustness/generated/ext4-base.img 2>/dev/null | grep '"outcome"'
```
Expected, for both:
`{"type":"outcome","class":"ok","lastStep":"close","reason":null,"errors":[],"stats":{"parts":1,"entries":11,"files":5,"bytes":71730}}`

That is 11 entries (`hello.txt docs data link lost+found readme.md nested blob.bin small.bin deep
leaf.txt`), 5 files, and 71730 bytes (13 + 2080 + 65536 + 4096 + 5). If the native line is missing
because the process got stuck after the outcome, check that `report()` reaches `process.exit(0)`.

- [ ] **Step 3: Smoke a broken case**

Run: `node ts/tests/robustness/case-runner.mjs --backend wasm --image ~/.cache/anyfs-robustness/generated/ext4-trunc-25.img 2>/dev/null | grep '"outcome"'`
Expected: `"class":"error"` with `"lastStep":"enter:0"` (ext4 rejects the truncated geometry at mount).

- [ ] **Step 4: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness/case-runner.mjs
git add ts/tests/robustness/case-runner.mjs
git commit -m "test(robustness): per-case runner for the wasm and native backends

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: `run.mjs`

**Files:**
- Create: `ts/tests/robustness/run.mjs`
- Create: `ts/tests/robustness/FINDINGS.md`

- [ ] **Step 1: Create the orchestrator**

`ts/tests/robustness/run.mjs`:

```js
#!/usr/bin/env node
/**
 * Robustness gate: no corrupt image may hang or crash the wasm sandbox.
 *   node ts/tests/robustness/run.mjs --backend wasm|native [--only <glob>[,…]] [--jobs N]
 * Generates the corpus if missing, fetches the syzbot set, runs every case in
 * its own case-runner.mjs process, writes
 * ~/.cache/anyfs-robustness/report-<backend>[-partial].json and prints a
 * summary. Exit: 1 when the wasm gate fails; 2 when setup failed or the
 * harness is broken (a case failed before touching its image).
 */
import { fork, spawnSync } from 'node:child_process';
import { createWriteStream, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { availableParallelism } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';
import { fetchSyzbot } from './fetch-syzbot.mjs';
import { finalClass, gate, harnessFailures } from './lib/classify.mjs';
import { matcher } from './lib/glob.mjs';
import {
    CASES_JSON,
    CORE_DIST,
    LOG_DIR,
    NATIVE_ADDON,
    ROBUSTNESS_DIR,
    WASM_NODE_BUNDLE,
    reportPath,
} from './lib/paths.mjs';
import { diffRuns, formatSummary } from './lib/report.mjs';

/** Per-case outer bound: past it the op watchdog failed to catch a wedge. */
const CASE_TIMEOUT_MS = 3 * 60_000;
/** After an outcome, how long a child may take to exit before it is killed. */
const EXIT_GRACE_MS = 15_000;

function die(msg) {
    console.error(msg);
    process.exit(2);
}

const { values: opts } = parseArgs({
    options: {
        backend: { type: 'string' },
        only: { type: 'string' },
        jobs: { type: 'string', default: String(Math.max(1, Math.min(4, availableParallelism() >> 1))) },
    },
});
const backend = opts.backend;
if (backend !== 'wasm' && backend !== 'native') {
    die('usage: run.mjs --backend wasm|native [--only <glob>[,<glob>…]] [--jobs N]');
}
const jobs = Number.parseInt(opts.jobs, 10);
if (!(jobs >= 1)) die(`--jobs: expected a positive integer, got ${opts.jobs}`);
const only = opts.only ? matcher(opts.only) : null;

const need = [[join(CORE_DIST, 'index.js'), 'pnpm -C ts -F @anyfs/core build']];
if (backend === 'wasm') need.push([WASM_NODE_BUNDLE, 'ANYFS_TARGET=node scripts/build_anyfs_wasm.sh']);
else need.push([NATIVE_ADDON, 'ts/packages/anyfs-native/scripts/build-linux-electron.sh']);
for (const [file, how] of need) if (!existsSync(file)) die(`missing ${file} — build it with: ${how}`);

// 1. The generated corpus.
if (!existsSync(CASES_JSON)) {
    const r = spawnSync(process.execPath, [join(ROBUSTNESS_DIR, 'corpus/generate.mjs')], {
        stdio: 'inherit',
    });
    if (r.status !== 0) die('corpus generation failed');
}
const generated = JSON.parse(readFileSync(CASES_JSON, 'utf-8')).cases;

// 2. The syzbot set.
let syzbot;
try {
    syzbot = await fetchSyzbot({ only });
} catch (e) {
    die(`syzbot fetch failed: ${e.message}`);
}

// 3. One child per case.
const cases = [...generated, ...syzbot].filter((c) => !only || only(c.name));
if (cases.length === 0) die(`no case matches --only ${opts.only}`);
mkdirSync(join(LOG_DIR, backend), { recursive: true });

function runCase(c) {
    return new Promise((resolve) => {
        const t0 = Date.now();
        const logFile = join(LOG_DIR, backend, `${c.name}.log`);
        const log = createWriteStream(logFile);
        const child = fork(
            join(ROBUSTNESS_DIR, 'case-runner.mjs'),
            ['--backend', backend, '--image', c.file],
            { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] },
        );
        child.stdout.pipe(log, { end: false });
        child.stderr.pipe(log, { end: false });
        let lastStep = 'spawn';
        let outcome = null;
        let timedOut = false;
        let killedAfterOutcome = false;
        let grace = null;
        const timer = setTimeout(() => {
            timedOut = true;
            child.kill('SIGKILL');
        }, CASE_TIMEOUT_MS);
        child.on('message', (m) => {
            if (m?.type === 'step') {
                lastStep = m.step;
            } else if (m?.type === 'outcome' && !outcome) {
                outcome = m;
                clearTimeout(timer);
                grace = setTimeout(() => {
                    killedAfterOutcome = true;
                    child.kill('SIGKILL');
                }, EXIT_GRACE_MS);
            }
        });
        child.on('close', (code, signal) => {
            clearTimeout(timer);
            clearTimeout(grace);
            log.end();
            resolve({
                name: c.name,
                source: c.source,
                fs: c.fs,
                mutation: c.mutation,
                sha256: c.sha256,
                backend,
                class: finalClass({ outcome, timedOut }),
                lastStep: outcome?.lastStep ?? lastStep,
                durationMs: Date.now() - t0,
                reason:
                    outcome?.reason ??
                    (timedOut
                        ? `no outcome within ${CASE_TIMEOUT_MS / 1000}s`
                        : `exited without an outcome (code ${code}, signal ${signal})`),
                errors: outcome?.errors ?? [],
                stats: outcome?.stats ?? null,
                exit: { code, signal, killedAfterOutcome },
                log: logFile,
            });
        });
    });
}

async function pool(items, n, fn) {
    const out = new Array(items.length);
    let next = 0;
    const worker = async () => {
        while (next < items.length) {
            const i = next++;
            out[i] = await fn(items[i]);
        }
    };
    await Promise.all(Array.from({ length: Math.min(n, items.length) }, worker));
    return out;
}

let done = 0;
const records = await pool(cases, jobs, async (c) => {
    const r = await runCase(c);
    done++;
    console.log(
        `[${String(done).padStart(3)}/${cases.length}] ${r.class.padEnd(5)} ${c.name.padEnd(35)} ${String(r.lastStep).padEnd(12)} ${(r.durationMs / 1000).toFixed(1)}s`,
    );
    return r;
});

const full = reportPath(backend);
const prev = existsSync(full) ? JSON.parse(readFileSync(full, 'utf-8')) : null;
const flips = diffRuns(prev, records);
const verdict = gate(records, backend);
const broken = harnessFailures(records);
const out = reportPath(backend, only !== null);
writeFileSync(
    out,
    `${JSON.stringify({ backend, finishedAt: new Date().toISOString(), partial: only !== null, records }, null, 4)}\n`,
);
console.log(formatSummary({ backend, records, verdict, flips, partial: only !== null }));
console.log(`\nreport: ${out}\nlogs:   ${join(LOG_DIR, backend)}`);
if (broken.length > 0) {
    console.error(
        `\nHARNESS BROKEN: these cases failed before touching the image:\n${broken.map((r) => `  ${r.name}: ${r.reason}`).join('\n')}`,
    );
    process.exit(2);
}
process.exit(backend === 'wasm' && !verdict.pass ? 1 : 0);
```

- [ ] **Step 2: Create `FINDINGS.md`**

`ts/tests/robustness/FINDINGS.md`:

```markdown
# Robustness gate: findings

Results of `ts/tests/robustness/run.mjs` that are not a plain `ok` / `error` / `fatal`:
native hangs and crashes (recorded, not gated), and anything the wasm gate caught and was fixed.

Reproduce one case with `node ts/tests/robustness/run.mjs --backend <backend> --only <case>`. Its
kernel log is in `~/.cache/anyfs-robustness/logs/<backend>/<case>.log`.

Status: OPEN · FIXED (commit)

## Native backend (non-gating)

| case | class | last step | reason | status |
|---|---|---|---|---|

## wasm gate

| case | class | last step | root cause | status |
|---|---|---|---|---|
```

- [ ] **Step 3: Partial run on the bases**

Run: `node ts/tests/robustness/run.mjs --backend wasm --only '*-base'`
Expected: 16 lines, a summary with `ok` for every base, `PARTIAL RUN`, exit 0, and
`report-wasm-partial.json` written. If a base is not `ok`, that is a real problem: read its log. One
known risk: if `xfs-base` fails with a log-size error, raise its size in `BASES.xfs` to 300 MiB (it is
sparse, so this costs no disk), regenerate with `--force`, and rerun.

- [ ] **Step 4: Commit**

```bash
pnpm -C ts exec prettier --write tests/robustness/run.mjs tests/robustness/FINDINGS.md
git add ts/tests/robustness/run.mjs ts/tests/robustness/FINDINGS.md
git commit -m "test(robustness): run.mjs — parallel per-case children, report, gate

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: Baseline before the mount hardening

**Files:** none (records results for the next task)

- [ ] **Step 1: Full wasm and native runs on the current build**

Run:
```bash
node ts/tests/robustness/run.mjs --backend wasm   | tee ~/.cache/anyfs-robustness/baseline-wasm.txt
node ts/tests/robustness/run.mjs --backend native | tee ~/.cache/anyfs-robustness/baseline-native.txt
```
Expected: both complete and write their reports. The wasm run may fail the gate at this point, which
is fine for a baseline.

- [ ] **Step 2: Confirm the panic reproduction**

Run: `grep -E 'ext4panic-zero-docs-inode' ~/.cache/anyfs-robustness/baseline-*.txt`
Expected:
- wasm: `fatal`, last step `walk:0`, reason starting with `wasm module aborted:` or `host-error:` (the
  kernel panicked);
- native: `crash` (SIGABRT from LKL's panic → `abort()`).

LKL's host `panic()` is `assert(0)`. The shipped wasm keeps its asserts (the bundle contains the
`posix-host.c` assert string), so a panic aborts. A wasm reason of `… timed out after 20s` would mean
the assert was compiled out and the panic loop wedged the API thread instead. That is still a
`fatal`, but note it in `FINDINGS.md`.

If wasm shows `error` instead, the image does not reproduce the panic. Investigate before going on: run
the case directly and read its kernel log (`EXT4-fs error … panic forced after error`), and check that
`ext4panic-base` has `Errors behavior: Panic` (`/sbin/debugfs -R stats <file>`).

- [ ] **Step 3: Note the syzbot ext4 `errors=panic` entry's baseline class**

Run: `grep -E '^\S+\s+syz-ext4' ~/.cache/anyfs-robustness/baseline-wasm.txt`
Note the class of the entry you picked for `sb_errors=3` in Task 10, for comparison in Task 15.

---

### Task 15: Mount hardening (`errors=continue`)

**Files:**
- Create: `src/core/anyfs_mount_opts.h`
- Create: `src/core/anyfs_mount_opts.c`
- Create: `tests/unit/test_mount_opts.c`
- Modify: `src/core/anyfs_mount.c`
- Modify: `meson.build:250-265` (core sources) and after line 396 (unit test)
- Modify: `scripts/build_anyfs_wasm.sh:112-127` (`CORE_SOURCES`)

- [ ] **Step 1: Write the failing C test**

`tests/unit/test_mount_opts.c`:

```c
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the per-filesystem mount options (src/core/anyfs_mount_opts.c). */
#include "anyfs_mount_opts.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

static void expect(const char *fstype, int rdonly, const char *want)
{
	char buf[64];
	int rc = anyfs_mount_opts(fstype, rdonly, buf, sizeof(buf));

	if (rc != 0 || strcmp(buf, want) != 0) {
		fprintf(stderr, "FAIL %s rdonly=%d: rc=%d got \"%s\", want \"%s\"\n",
			fstype, rdonly, rc, buf, want);
		failures++;
	}
}

int main(void)
{
	char buf[8];

	/* A superblock must not be able to ask for a panic. */
	expect("ext4", 1, "noload,errors=continue");
	expect("ext4", 0, "errors=continue");
	expect("ext3", 1, "noload,errors=continue");
	expect("ext2", 1, "errors=continue");
	expect("vfat", 1, "errors=continue");
	expect("msdos", 1, "errors=continue");
	expect("exfat", 1, "errors=continue");
	expect("f2fs", 1, "errors=continue");
	expect("ntfs", 1, "errors=continue");

	/* No errors= option: unchanged. */
	expect("xfs", 1, "norecovery");
	expect("btrfs", 1, "norecovery");
	expect("btrfs", 0, "");
	expect("iso9660", 1, "");
	expect("hfsplus", 1, "");
	expect("apfs", 1, "");
	expect("ufs", 1, "ufstype=ufs2");
	expect("ufs", 0, "ufstype=ufs2");

	CHECK(anyfs_mount_opts(NULL, 1, buf, sizeof(buf)) == 0 && buf[0] == '\0');
	/* Too small: fails and leaves an empty string. */
	CHECK(anyfs_mount_opts("ext4", 1, buf, sizeof(buf)) == -1 && buf[0] == '\0');
	CHECK(anyfs_mount_opts("ext4", 1, buf, 0) == -1);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("mount_opts: all passed\n");
	return 0;
}
```

In `meson.build`, after the `test('tls_ca', …)` line, add:

```meson
test_mount_opts = executable('test_mount_opts',
    'tests/unit/test_mount_opts.c',
    'src/core/anyfs_mount_opts.c',
    include_directories: [include_directories('src/core')],
    install: false,
)
test('mount_opts', test_mount_opts, suite: 'unit')
```

- [ ] **Step 2: Run it to verify it fails**

Run: `ninja -C build-anyfs-linux-amd64 test_mount_opts`
Expected: FAIL. meson regenerates and reports that `src/core/anyfs_mount_opts.c` does not exist.

- [ ] **Step 3: Implement the helper**

`src/core/anyfs_mount_opts.h`:

```c
/*
 * anyfs_mount_opts.h — mount options anyfs adds per filesystem (internal)
 */
#ifndef ANYFS_MOUNT_OPTS_H
#define ANYFS_MOUNT_OPTS_H

#include <stddef.h>

/* Write the comma-separated mount options for `fstype` into buf (cap bytes,
 * always NUL-terminated; "" when there are none). `rdonly` is non-zero for a
 * read-only mount. Returns 0, or -1 if the options don't fit. */
int anyfs_mount_opts(const char* fstype, int rdonly, char* buf, size_t cap);

#endif /* ANYFS_MOUNT_OPTS_H */
```

`src/core/anyfs_mount_opts.c`:

```c
/*
 * anyfs_mount_opts.c — mount options anyfs adds per filesystem (internal)
 *
 * Pure string logic, kept apart from anyfs_mount.c so it builds and is
 * unit-tested without LKL.
 */
#include "anyfs_mount_opts.h"

#include <stdio.h>
#include <string.h>

/* Filesystems whose on-disk state can choose the kernel's reaction to an
 * error, panic included (ext*'s superblock s_errors; the others take the
 * option too). anyfs opens images users did not make, so it always mounts
 * these with errors=continue: the error surfaces as EIO and the kernel
 * lives on. */
static const char* const errors_continue_fs[] = {
	"ext2", "ext3", "ext4", "vfat", "msdos", "exfat", "f2fs", "ntfs", NULL,
};

static int in_list(const char* fstype, const char* const* list)
{
	for (; *list; list++)
		if (strcmp(fstype, *list) == 0)
			return 1;
	return 0;
}

static int append(char* buf, size_t cap, size_t* len, const char* opt)
{
	int n = snprintf(buf + *len, cap - *len, "%s%s", *len ? "," : "", opt);

	if (n < 0 || (size_t)n >= cap - *len)
		return -1;
	*len += (size_t)n;
	return 0;
}

int anyfs_mount_opts(const char* fstype, int rdonly, char* buf, size_t cap)
{
	size_t len = 0;
	int rc = 0;

	if (!buf || cap == 0)
		return -1;
	buf[0] = '\0';
	if (!fstype)
		return 0;

	if (rdonly) {
		/* Never replay a journal or log into a read-only image. */
		if (strcmp(fstype, "xfs") == 0 || strcmp(fstype, "btrfs") == 0)
			rc |= append(buf, cap, &len, "norecovery");
		else if (strcmp(fstype, "ext4") == 0 ||
			 strcmp(fstype, "ext3") == 0)
			rc |= append(buf, cap, &len, "noload");
	}
	/* The Linux UFS driver defaults to 44bsd UFS1, which matches nothing
	 * modern; FreeBSD, NetBSD and OpenBSD all ship UFS2. */
	if (strcmp(fstype, "ufs") == 0)
		rc |= append(buf, cap, &len, "ufstype=ufs2");
	if (in_list(fstype, errors_continue_fs))
		rc |= append(buf, cap, &len, "errors=continue");

	if (rc) {
		buf[0] = '\0';
		return -1;
	}
	return 0;
}
```

Add `'src/core/anyfs_mount_opts.c',` to `anyfs_core_sources` in `meson.build`, right after
`'src/core/anyfs_mount.c',`. Add `anyfs_mount_opts.c` to `CORE_SOURCES` in
`scripts/build_anyfs_wasm.sh`, right after `anyfs_mount.c`.

- [ ] **Step 4: Run the C test to verify it passes**

Run: `ninja -C build-anyfs-linux-amd64 test_mount_opts && meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs mount_opts`
Expected: `1/1 mount_opts OK`.

- [ ] **Step 5: Use it in `anyfs_mount.c`**

In `src/core/anyfs_mount.c`, add `#include "anyfs_mount_opts.h"` after `#include "anyfs.h"`.

In `mount_via_devpath()`, add a buffer after `int ret;`:

```c
	int ret;
	char opts[64];
```

Replace the explicit-fstype block from `const char* opts = NULL;` through the `lkl_sys_mount(...)` call:

```c
		const char* opts = NULL;
		if (flags & ANYFS_MOUNT_RDONLY) {
			if (strcmp(fstype, "xfs") == 0 ||
			    strcmp(fstype, "btrfs") == 0)
				opts = "norecovery";
			else if (strcmp(fstype, "ext4") == 0 ||
				 strcmp(fstype, "ext3") == 0)
				opts = "noload";
		}
		// The Linux UFS driver needs ufstype= to pick the right
		// superblock layout. Default is 44bsd UFS1 (ufstype=old) which
		// fails on every modern image. FreeBSD/NetBSD/OpenBSD all ship
		// ufs2 today.
		if (strcmp(fstype, "ufs") == 0 && !opts)
			opts = "ufstype=ufs2";
		ret = lkl_sys_mount((char*)dev_str, mnt, (char*)fstype,
				    mount_flags, (char*)opts);
```

with:

```c
		if (anyfs_mount_opts(fstype, flags & ANYFS_MOUNT_RDONLY, opts,
				     sizeof(opts)) < 0) {
			lkl_sys_rmdir(mnt);
			return -1;
		}
		ret = lkl_sys_mount((char*)dev_str, mnt, (char*)fstype,
				    mount_flags, opts[0] ? opts : NULL);
```

In the blind-probe loop, replace from `const char* opts = NULL;` through its `lkl_sys_mount(...)` call:

```c
		const char* opts = NULL;
		if (flags & ANYFS_MOUNT_RDONLY) {
			if (strcmp(fstypes[i], "xfs") == 0 ||
			    strcmp(fstypes[i], "btrfs") == 0)
				opts = "norecovery";
			else if (strcmp(fstypes[i], "ext4") == 0 ||
				 strcmp(fstypes[i], "ext3") == 0)
				opts = "noload";
		}
		// ufstype=ufs2 covers modern FreeBSD/NetBSD/OpenBSD; the
		// default 44bsd layout doesn't match anything you'd actually
		// mount today.
		if (strcmp(fstypes[i], "ufs") == 0 && !opts)
			opts = "ufstype=ufs2";
		ret = lkl_sys_mount((char*)dev_str, mnt, fstypes[i],
				    mount_flags, (char*)opts);
```

with:

```c
		if (anyfs_mount_opts(fstypes[i], flags & ANYFS_MOUNT_RDONLY,
				     opts, sizeof(opts)) < 0)
			continue;
		ret = lkl_sys_mount((char*)dev_str, mnt, fstypes[i],
				    mount_flags, opts[0] ? opts : NULL);
```

- [ ] **Step 6: Rebuild every artifact**

Run, in order:
```bash
./scripts/build_anyfs.sh --targets=linux-amd64 --components=core,server
meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs
ts/packages/anyfs-native/scripts/build-linux-electron.sh
ANYFS_TARGET=node ./scripts/build_anyfs_wasm.sh
ANYFS_TARGET=browser ./scripts/build_anyfs_wasm.sh
./scripts/sync_wasm_bundle.sh
```
Expected: every step succeeds and every unit test is OK.

Verify that each artifact carries the new code (stale bundles have bitten this repo before):
```bash
for f in build-anyfs-linux-amd64/libanyfs_core.a ts/packages/anyfs-native/build/Release/anyfs_native.node \
         ts/packages/core/wasm/anyfs.node.wasm ts/packages/core/wasm/anyfs.wasm ts/examples/vite-demo/public/wasm/anyfs.wasm; do
  printf '%-60s %s\n' "$f" "$(strings "$f" | grep -c 'errors=continue')"; done
```
Expected: every count ≥ 1.

- [ ] **Step 7: Verify the hardening end to end**

Run: `node ts/tests/robustness/run.mjs --backend wasm --only '*-base,ext4panic-*,syz-ext4-*'`
Expected: every `*-base` is `ok` (ext4, ext2, vfat, exfat, f2fs and ntfs now mount with
`errors=continue`); `ext4panic-zero-docs-inode` is now **`error`** (it was `fatal` in Task 14); the
syzbot `sb_errors=3` entry no longer ends in a panic fatal.

Run: `node ts/tests/robustness/run.mjs --backend native --only '*-base,ext4panic-*'`
Expected: bases `ok`; `ext4panic-zero-docs-inode` is now `error` (it was `crash`).

- [ ] **Step 8: Commit**

```bash
git add src/core/anyfs_mount_opts.h src/core/anyfs_mount_opts.c src/core/anyfs_mount.c tests/unit/test_mount_opts.c meson.build scripts/build_anyfs_wasm.sh
git commit -m "fix(core): mount with errors=continue so an image can't request a panic

ext4 honours a superblock errors=panic even on a read-only mount, so a
hostile image could panic LKL on its first error. ext2/3/4, vfat/msdos,
exfat, f2fs and NTFS PLUS now always get errors=continue (OOT APFS has
no such option). The per-fs option logic moves into a pure helper with
a unit test; the robustness corpus checks every unmutated base still
mounts.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 16: Gate runs and findings

**Files:**
- Modify: `ts/tests/robustness/FINDINGS.md`

- [ ] **Step 1: Full wasm gate run**

Run: `node ts/tests/robustness/run.mjs --backend wasm; echo exit=$?`
Expected: `gate passed`, `exit=0`.

If the gate fails, then for **each** `hang` or `crash`:
1. Use superpowers:systematic-debugging. Reproduce with `--only <case>`, read
   `~/.cache/anyfs-robustness/logs/wasm/<case>.log`, and run `case-runner.mjs` directly for the
   JSON-line trace.
2. Fix the root cause in the product (anyfs C glue, session layer or worker), not in the harness. Do
   not loosen the classification or the limits to get a pass.
3. Add a row to the "wasm gate" table in `FINDINGS.md`: case, class, last step, root cause, and
   `FIXED (<commit>)`.
4. Commit the fix with its own message, then rerun the full gate.

A `fatal` is allowed by the gate, but check that its reason makes sense (a panic, abort or watchdog
message).

- [ ] **Step 2: Determinism check**

Run: `node ts/tests/robustness/run.mjs --backend wasm`
Expected: the gate passes again and there is no `class changed since the previous run` section. If a
case flips, add it to `FINDINGS.md` as a finding: a flipping case is itself a result.

- [ ] **Step 3: Native report**

Run: `node ts/tests/robustness/run.mjs --backend native; echo exit=$?`
Expected: `exit=0` (native never fails the run). Copy every `hang` and `crash` row from the summary
into the "Native backend" table of `FINDINGS.md`, with status `OPEN`. If there are none, write
`No hangs or crashes on <date> (<N> cases).` under the table.

- [ ] **Step 4: Commit**

```bash
git add ts/tests/robustness/FINDINGS.md
git commit -m "docs(robustness): record the native findings of the first full gate run

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 17: A visible readdir error in the file browser

**Files:**
- Modify: `ts/packages/trees/src/AnyfsFileBrowser.tsx:167-169,223-240,436-452`
- Modify: `ts/tests/e2e/drivers/driver.ts`
- Modify: `ts/tests/e2e/drivers/dom-actions.ts:197-218`
- Modify: `ts/tests/e2e/drivers/web-driver.ts`, `ts/tests/e2e/drivers/electron-driver.ts`

- [ ] **Step 1: Render readdir failures**

In `ts/packages/trees/src/AnyfsFileBrowser.tsx`, after `const [files, setFiles] = useState<FileArray>([null]);`
add:

```tsx
    const [dirError, setDirError] = useState<string | null>(null);
```

In the readdir effect, after `setFiles([null, null, null]); // chonky shows a loader skeleton` add:

```tsx
        setDirError(null);
```

and in its `catch (err)` block, before `setFiles([]);`, add:

```tsx
                setDirError(err instanceof Error ? err.message : String(err));
```

In the returned JSX, insert this right before `<FileBrowser`:

```tsx
            {dirError && (
                // A corrupt directory used to look like an empty one.
                <div
                    role="alert"
                    data-testid="dir-error"
                    style={{
                        margin: '0 0 6px',
                        padding: '6px 10px',
                        borderRadius: 6,
                        fontSize: 13,
                        color: darkMode ? '#fca5a5' : '#b91c1c',
                        background: darkMode ? 'rgba(127, 29, 29, 0.35)' : '#fee2e2',
                    }}
                >
                    Can’t read this folder: {dirError}
                </div>
            )}
```

- [ ] **Step 2: Teach the E2E drivers about it**

In `ts/tests/e2e/drivers/driver.ts`, change `ErrorKind` to:

```ts
export type ErrorKind = 'bad-image' | 'no-range' | 'unsupported' | 'mount-failed' | 'read-failed';
```

and add to the `Driver` interface, after `backendMode()`:

```ts
    /** The provider status from the test bridge ('ready', 'error', …). */
    status(): Promise<string | null>;
```

In `ts/tests/e2e/drivers/dom-actions.ts`, extend the race in `expectError` with a fourth entry and
update its comment's list:

```ts
        page
            .locator('[data-testid="dir-error"]')
            .waitFor({ state: 'visible', timeout: 120_000 }),
```

(The comment gains a line: "a failed directory read renders inline in the file browser
(data-testid dir-error)".) Then add, after `closeDisk`:

```ts
/** The provider status the test bridge reports ('ready', 'error', …). */
export async function getStatus(page: Page): Promise<string | null> {
    return page.evaluate(() => (window as any).__anyfsTest?.getState().status ?? null);
}
```

In both `web-driver.ts` and `electron-driver.ts`, add next to `expectError`:

```ts
    async status(): Promise<string | null> {
        return dom.getStatus(this.page);
    }
```

- [ ] **Step 3: Build to type-check**

Run: `pnpm -C ts build && pnpm -C ts -F vite-demo build && (cd ts/tests/e2e && npx tsc --noEmit -p .)`
Expected: no errors.

- [ ] **Step 4: Commit**

```bash
pnpm -C ts exec prettier --write packages/trees/src/AnyfsFileBrowser.tsx tests/e2e/drivers
git add ts/packages/trees/src/AnyfsFileBrowser.tsx ts/tests/e2e/drivers
git commit -m "feat(trees): show a failed directory read instead of an empty folder

E2E drivers gain status() and a read-failed error surface.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 18: Robustness E2E cases

**Files:**
- Create: `ts/tests/e2e/fixtures/robustness.ts`
- Create: `ts/tests/e2e/flows/robustness.spec.ts`
- Modify: `ts/tests/e2e/playwright.config.ts` (electron-native project)

- [ ] **Step 1: Pick the cases from the wasm report**

Run:
```bash
node -e '
const r = require(process.env.HOME + "/.cache/anyfs-robustness/report-wasm.json");
const show = (x) => console.log(x.class.padEnd(6), x.name.padEnd(36), String(x.lastStep).padEnd(10), x.reason);
for (const n of ["ext4-trunc-25", "ext4-zero-docs-inode"]) show(r.records.find((x) => x.name === n));
console.log("--- fatal candidates");
r.records.filter((x) => x.class === "fatal").forEach(show);
'
```
Expected: `ext4-trunc-25` is `error` at `enter:0`. `ext4-zero-docs-inode` is `error` with last step
`walk:0`/`read:0` and a reason naming `docs`. If either differs, pick another generated case with that
behaviour from the report and use its name below.

Choose the fatal case with these rules, in order:
1. reason starts with `wasm module aborted` or `host-error` (a panic or trap, not the watchdog);
2. last step is `enter:N` (the UI mounts on click; a panic deep in the walk may not trigger in the UI,
   which only lists one directory) or `open` (fatal during attach);
3. prefer `source: syzbot`, then the alphabetically first name.

Set `FATAL_CASE` to its name. Set `FATAL_PART` to the `N` of `enter:N`, or `null` for `open`.

Fallbacks: if no candidate meets rule 1, take a watchdog fatal (reason `… timed out after 20s`) with
last step `enter:N`. The product watchdog is 60 s, so add `test.setTimeout(300_000)` to that test. If
there is no fatal case at all, stop and ask the user.

- [ ] **Step 2: Create the fixture helper**

`ts/tests/e2e/fixtures/robustness.ts`:

```ts
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { TS_ROOT } from '../lib/paths';
import type { Fixture } from './manifest';

const ROBUSTNESS_DIR = resolve(TS_ROOT, 'tests/robustness');
const CACHE_DIR =
    process.env.ANYFS_ROBUSTNESS_DIR ??
    join(process.env.XDG_CACHE_HOME ?? join(homedir(), '.cache'), 'anyfs-robustness');

/** A robustness-corpus case (ts/tests/robustness) as an E2E fixture.
 *  Generates the corpus, or fetches the one syzbot image, on first use. */
export function robustnessCase(name: string): Fixture {
    const file = name.startsWith('syz-') ? syzbotFile(name) : generatedFile(name);
    return { name, source: 'generated', file, parts: [] };
}

function generatedFile(name: string): string {
    const casesJson = join(CACHE_DIR, 'generated', 'cases.json');
    if (!existsSync(casesJson)) {
        execFileSync(process.execPath, [join(ROBUSTNESS_DIR, 'corpus/generate.mjs')], {
            stdio: 'inherit',
        });
    }
    const { cases } = JSON.parse(readFileSync(casesJson, 'utf-8')) as {
        cases: { name: string; file: string }[];
    };
    const c = cases.find((x) => x.name === name);
    if (!c) throw new Error(`robustness case ${name} is not in ${casesJson}`);
    return c.file;
}

function syzbotFile(name: string): string {
    const out = execFileSync(
        process.execPath,
        [join(ROBUSTNESS_DIR, 'fetch-syzbot.mjs'), '--only', name],
        { encoding: 'utf-8' },
    );
    const line = out.split('\n').find((l) => l.startsWith(`${name}\t`));
    if (!line) throw new Error(`syzbot case ${name} is not in syzbot.json`);
    return line.split('\t')[1]!;
}
```

- [ ] **Step 3: Write the spec**

`ts/tests/e2e/flows/robustness.spec.ts`. Fill in `FATAL_CASE`, `FATAL_PART` and the one-line reason
from Step 1:

```ts
import { test, expect } from '../lib/test-fixture';
import type { Driver } from '../drivers/driver';
import { ensureFixture } from '../fixtures/ensure';
import { robustnessCase } from '../fixtures/robustness';

// Corrupt images from the robustness corpus (ts/tests/robustness). What is
// tested is the wasm sandbox promise: a hostile image ends in a visible
// error or a reported fatal, never a hang, and the app then opens the next
// image normally. Web and electron-wasm only: native crashes are findings
// (ts/tests/robustness/FINDINGS.md), so playwright.config.ts keeps this file
// off the electron-native project.

/** Truncated to 25 %: ext4 rejects the geometry at mount. */
const ERROR_CASE = 'ext4-trunc-25';
/** Mounts and lists its root, but the docs/ inode is zeroed. */
const READ_CASE = 'ext4-zero-docs-inode';
/** From report-wasm.json: <reason from Step 1>. */
const FATAL_CASE = '<name from Step 1>';
/** Partition whose mount trips the fatal; null when it hits during open. */
const FATAL_PART: number | null = null; // <N from Step 1, or null>

const good = ensureFixture('singleExt4');

/** After any failure: close, open a good image, see its partitions. */
async function expectRecovery(driver: Driver): Promise<void> {
    await driver.close();
    await driver.openImage(good);
    await expect.poll(() => driver.listPartitionIndices(), { timeout: 90_000 }).toEqual([0]);
}

test('corrupt image that cannot mount: clean error, session stays healthy', async ({ driver }) => {
    await driver.openImage(robustnessCase(ERROR_CASE));
    expect(await driver.listPartitionIndices()).toEqual([0]);
    // enterPartition waits for a file list that never comes; don't await it.
    void driver.enterPartition(0).catch(() => {});
    await driver.expectError('mount-failed');
    expect(await driver.status()).toBe('ready');
    await expectRecovery(driver);
});

test('image that mounts but fails on read: error shown, session stays healthy', async ({
    driver,
}) => {
    await driver.openImage(robustnessCase(READ_CASE));
    expect(await driver.listPartitionIndices()).toEqual([0]);
    await driver.enterPartition(0);
    expect((await driver.listRows()).map((r) => r.name)).toContain('docs');
    await driver.navigateInto('docs');
    await driver.expectError('read-failed');
    expect(await driver.status()).toBe('ready');
    await expectRecovery(driver);
});

test('image that crashes the kernel: reported fatal, then recovery', async ({ driver }) => {
    await driver.openImage(robustnessCase(FATAL_CASE));
    if (FATAL_PART !== null) {
        await expect
            .poll(() => driver.listPartitionIndices(), { timeout: 90_000 })
            .toContain(FATAL_PART);
        void driver.enterPartition(FATAL_PART).catch(() => {});
    }
    // The worker aborts; the session fires onFatal and the provider flips the
    // whole disk to status 'error', not just an inline mount error.
    await expect.poll(() => driver.status(), { timeout: 120_000 }).toBe('error');
    await expectRecovery(driver);
});
```

In `ts/tests/e2e/playwright.config.ts`, give the `electron-native` project a `testIgnore`:

```ts
        {
            name: 'electron-native',
            testMatch: ['flows/**/*.spec.ts', 'electron-only/**/*.spec.ts'],
            // Native crashes on corrupt images are recorded findings, not gated
            // (ts/tests/robustness/FINDINGS.md).
            testIgnore: ['flows/robustness.spec.ts'],
        },
```

- [ ] **Step 4: Run on web and electron-wasm**

Run:
```bash
pnpm -C ts build
cd ts/tests/e2e
npx playwright test flows/robustness.spec.ts --project=web
xvfb-run -a npx playwright test flows/robustness.spec.ts --project=electron-wasm
cd -
```
Expected: 3 passed on each project. If the electron project needs the electron-demo build first, follow
`ts/tests/e2e/README.md`.

- [ ] **Step 5: Regression check on the neighbouring specs**

Run:
```bash
cd ts/tests/e2e
npx playwright test flows/errors.spec.ts flows/switch.spec.ts flows/open-browse-download.spec.ts --project=web
xvfb-run -a npx playwright test flows/errors.spec.ts flows/switch.spec.ts --project=electron-wasm --project=electron-native
cd -
```
Expected: same pass/skip counts as before these changes (no new failures).

- [ ] **Step 6: Commit**

```bash
pnpm -C ts exec prettier --write tests/e2e/fixtures/robustness.ts tests/e2e/flows/robustness.spec.ts tests/e2e/playwright.config.ts
git add ts/tests/e2e/fixtures/robustness.ts ts/tests/e2e/flows/robustness.spec.ts ts/tests/e2e/playwright.config.ts
git commit -m "test(e2e): corrupt-image error, read failure and fatal, each with recovery

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 19: Docs, memory, final verification, push

**Files:**
- Create: `ts/tests/robustness/README.md`
- Modify: `docs/superpowers/specs/2026-10-05-robustness-gate-design.md:4` (status line)

- [ ] **Step 1: Write the README**

`ts/tests/robustness/README.md`:

````markdown
# Robustness gate

A local, pre-release check of anyfs's core promise: on the wasm backend, no corrupt, truncated or
hostile image makes the app hang or crash. Every failure ends as a clean error or a reported fatal.
CI does not run it. Design: `docs/superpowers/specs/2026-10-05-robustness-gate-design.md`.

## Prerequisites

- Image tools: `sudo apt-get install mtools xorriso exfatprogs` on top of e2fsprogs, dosfstools,
  f2fs-tools, ntfs-3g, btrfs-progs, xfsprogs, squashfs-tools, qemu-utils and fdisk. The generator
  names anything missing.
- Built artifacts: `pnpm -C ts -F @anyfs/core build`, the Node wasm bundle
  (`ANYFS_TARGET=node scripts/build_anyfs_wasm.sh`), and for `--backend native` the addon
  (`ts/packages/anyfs-native/scripts/build-linux-electron.sh`).
- Network on the first run, to fetch the syzbot images.

## Run

```sh
node ts/tests/robustness/run.mjs --backend wasm      # the gate: exit 1 on failure
node ts/tests/robustness/run.mjs --backend native    # report only; record findings
node ts/tests/robustness/run.mjs --backend wasm --only 'ext4-*,syz-ext4-*' --jobs 2
node ts/tests/robustness/case-runner.mjs --backend wasm --image <file>   # one case, JSON lines
```

Data lives in `~/.cache/anyfs-robustness/` (override with `ANYFS_ROBUSTNESS_DIR`):
`generated/` (78 cases + `cases.json`), `syzbot/`, `logs/<backend>/<case>.log` and
`report-<backend>.json` (`-partial` for `--only` runs). Regenerate the corpus with
`node ts/tests/robustness/corpus/generate.mjs --force`.

## Outcome classes

| class | meaning |
|---|---|
| `ok` | every step completed |
| `error` | an op failed with an ordinary error; the process stayed healthy |
| `fatal` | the session fired `onFatal` with a reason (panic → abort, the op watchdog, attach timeout) |
| `hang` | no outcome within 3 min: the watchdog missed a wedge |
| `crash` | the child died without an outcome |

wasm passes when nothing hangs or crashes, every fatal has a reason, and every unmutated base is
`ok`. Native hangs and crashes go in `FINDINGS.md`. A case whose class changes between two runs of
the same image is a finding too.

Notes:
- The Node harness mirrors the browser worker: an uncaught error on the module-owning thread counts
  as a fatal, as `worker.ts` turns it into `host-error`.
- syzbot images are mounted with anyfs's own options, not the reproducer's (`repro_mount_opts` in
  `syzbot.json` is informational).
- Curating the syzbot set: `tools/list-syzbot-candidates.mjs`, then `fetch-syzbot.mjs --pin`.
````

- [ ] **Step 2: Mark the spec implemented**

In `docs/superpowers/specs/2026-10-05-robustness-gate-design.md`, change line 4 to:

```markdown
**Status:** implemented (plan: `docs/superpowers/plans/2026-10-06-robustness-gate.md`)
```

- [ ] **Step 3: Final verification**

Use superpowers:verification-before-completion. Run all of these and read the output:
```bash
pnpm -C ts -F @anyfs/core build && pnpm -C ts -F @anyfs/core test:unit
node --test ts/tests/robustness/test/*.test.mjs
meson test -C build-anyfs-linux-amd64 --suite unit --print-errorlogs
node ts/tests/robustness/run.mjs --backend wasm; echo exit=$?
pnpm -C ts exec prettier --check tests/robustness tests/e2e/flows/robustness.spec.ts tests/e2e/fixtures/robustness.ts packages/core/src packages/core/test
```
Expected: everything passes; the gate prints `gate passed`, `exit=0`.

Map the results to the acceptance criteria in the spec:
1. wasm full run, no hang or crash, every base ok: the `run.mjs --backend wasm` output.
2. native report plus FINDINGS: Task 16 Step 3.
3. watchdog unit tests on WasmSession / NativeSession, abort → onFatal on NodeWasmSession: core
   `test:unit`.
4. hardened options keep bases mountable, and the `errors=panic` images no longer panic: Task 15
   Step 7 and the gate run.
5. E2E on web and electron-wasm: Task 18 Step 4.

- [ ] **Step 4: Commit the docs**

```bash
git add ts/tests/robustness/README.md docs/superpowers/specs/2026-10-05-robustness-gate-design.md
git commit -m "docs(robustness): README for the gate; mark the spec implemented

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Memory**

Write a project memory to `/home/kosaka/.claude/projects/-home-kosaka-anyfs-reader/memory/project_robustness_gate.md`
(frontmatter `name: project_robustness_gate`, `type: project`). Cover how to run the gate, where its
data lives, the counts from the first full run, the findings worth remembering, and anything
surprising from Tasks 14–18. Add one line under "Core / LKL (native)" in that directory's `MEMORY.md`.
Read `MEMORY.md` first and update an existing entry rather than duplicating one.

- [ ] **Step 6: Push only with consent**

Run: `git fetch origin && git log --oneline origin/main..main`
This lists this plan's commits and any other session's local commits. Show the list to the user (in
Chinese) and ask whether to push. Push only if they say yes: `git push origin main`.
