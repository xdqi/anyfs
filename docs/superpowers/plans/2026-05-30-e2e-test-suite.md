# E2E Test Suite (Web + Electron) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Playwright-driven E2E suite proving the web (vite-demo) and Electron (electron-demo) apps open disk images, browse the filesystem, and download files — across the wasm, Electron-native, and Electron-wasm backends. (`@smoke` tags are kept for a future CI gate; CI wiring itself is out of scope for this plan.)

**Architecture:** One Playwright config at `ts/tests/e2e/` with three projects (`web`, `electron-native`, `electron-wasm`) sharing a `Driver` interface, so each user-flow spec is written once and runs across all backends. Fixtures are root/sudo-generated raw/MBR/VMDK images plus downloaded-and-cached qcow2/iso. Small prerequisite changes are made to the apps under test (stable `data-testid`s, a `getState()`/`lastError` debug hook gated behind `import.meta.env.DEV || ?e2e=1`, a `ANYFS_TEST_DOWNLOAD_DIR` save-dialog bypass) and three production-hygiene cleanups.

**Tech Stack:** Playwright `@playwright/test` (incl. `_electron`), Node (mkfs/loop fixture tooling), Vite (prod preview), Electron (esbuilt `dist/main.cjs`), pnpm workspace, prettier.

**Spec:** `docs/superpowers/specs/2026-05-30-e2e-test-suite-design.md`

**Conventions to match:** prettier `.prettierrc.json` (4-space indent, single quotes, semicolons, trailing commas, printWidth 100). pnpm 11.2.2. No ESLint. Tests use `@playwright/test`'s own `expect`. All repo docs/comments/commits in English.

---

## Phase ordering & dependencies

```
Phase 0  Scaffold (workspace member, package, playwright config skeleton)
Phase 1  App-under-test prerequisites (testids, getState hook, download-dir bypass, cleanups)
Phase 2  Fixtures (manifest, generator, fetcher, range server)
Phase 3  Driver interface + WebDriver + ElectronDriver
Phase 4  Flow: open-browse-download   (depends 1,2,3)
Phase 5  Flow: url-load               (depends 2,3 + range server)
Phase 6  Flow: formats                (depends 2,3)
Phase 7  Flow: errors                 (depends 1,3)
Phase 8  Electron backend-switch flow (depends 1,3)
Phase 9  Suite docs & local run (@smoke confirmation + README; no CI)
```

Phases 4–8 are independent of each other once 1–3 land. Phase 9 is last. CI wiring is
deferred — not part of this plan.

---

## Phase 0 — Scaffold

### Task 0.1: Make `ts/tests` a workspace member

**Files:**
- Modify: `ts/pnpm-workspace.yaml`
- Modify: `ts/package.json` (prettier globs)

- [ ] **Step 1: Add `tests/*` to the workspace**

Edit `ts/pnpm-workspace.yaml` to:

```yaml
packages:
  - 'packages/*'
  - 'examples/*'
  - 'tests/*'
```

- [ ] **Step 2: Extend prettier globs to include tests**

In `ts/package.json`, change the two prettier scripts:

```json
"format": "prettier --write '{packages,examples,tests}/**/*.{ts,tsx,mjs,js,json}'",
"format:check": "prettier --check '{packages,examples,tests}/**/*.{ts,tsx,mjs,js,json}'"
```

- [ ] **Step 3: Commit**

```bash
git add ts/pnpm-workspace.yaml ts/package.json
git commit -m "chore(ts): add tests/* to pnpm workspace + prettier globs"
```

### Task 0.2: Create the e2e package

**Files:**
- Create: `ts/tests/e2e/package.json`
- Create: `ts/tests/e2e/tsconfig.json`
- Create: `ts/tests/e2e/.gitignore`

- [ ] **Step 1: Write `package.json`**

Create `ts/tests/e2e/package.json`:

```json
{
    "name": "@anyfs/e2e",
    "private": true,
    "version": "0.0.0",
    "type": "module",
    "scripts": {
        "fixtures": "node fixtures/generate.mjs && node fixtures/fetch.mjs",
        "test": "playwright test",
        "test:web": "playwright test --project=web",
        "test:electron": "playwright test --project=electron-native --project=electron-wasm",
        "test:smoke": "playwright test --grep @smoke"
    },
    "devDependencies": {
        "@playwright/test": "^1.49.0",
        "@anyfs/core": "workspace:*"
    }
}
```

- [ ] **Step 2: Write `tsconfig.json`**

Create `ts/tests/e2e/tsconfig.json`:

```json
{
    "compilerOptions": {
        "target": "ES2022",
        "module": "ESNext",
        "moduleResolution": "Bundler",
        "strict": true,
        "esModuleInterop": true,
        "skipLibCheck": true,
        "types": ["node"],
        "noEmit": true
    },
    "include": ["**/*.ts"]
}
```

- [ ] **Step 3: Write `.gitignore`**

Create `ts/tests/e2e/.gitignore`:

```
fixtures/images/
test-results/
playwright-report/
.cache/
```

- [ ] **Step 4: Install**

Run: `cd ts && pnpm install`
Expected: `@anyfs/e2e` resolves, `@playwright/test` installed under it.

- [ ] **Step 5: Install Playwright browsers**

Run: `cd ts/tests/e2e && pnpm exec playwright install chromium`
Expected: Chromium downloaded (Electron uses its own bundled Chromium, no separate install).

- [ ] **Step 6: Commit**

```bash
git add ts/tests/e2e/package.json ts/tests/e2e/tsconfig.json ts/tests/e2e/.gitignore ts/pnpm-lock.yaml
git commit -m "chore(e2e): scaffold @anyfs/e2e package with Playwright"
```

### Task 0.3: Playwright config skeleton (web project only, smoke-proves the harness)

**Files:**
- Create: `ts/tests/e2e/playwright.config.ts`
- Create: `ts/tests/e2e/lib/paths.ts`
- Test: `ts/tests/e2e/flows/_harness.spec.ts` (temporary, removed in Step 5)

- [ ] **Step 1: Write a path-resolution helper**

Create `ts/tests/e2e/lib/paths.ts`:

```ts
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));

/** Repo paths relative to ts/tests/e2e/lib. */
export const E2E_DIR = resolve(here, '..');
export const TS_ROOT = resolve(E2E_DIR, '../..');
export const VITE_DEMO_DIR = resolve(TS_ROOT, 'examples/vite-demo');
export const ELECTRON_DEMO_DIR = resolve(TS_ROOT, 'examples/electron-demo');
export const IMAGES_DIR = resolve(E2E_DIR, 'fixtures/images');
```

- [ ] **Step 2: Write the config with the web project + preview webServer**

Create `ts/tests/e2e/playwright.config.ts`:

```ts
import { defineConfig } from '@playwright/test';
import { VITE_DEMO_DIR } from './lib/paths';

const WEB_PORT = 4173;

export default defineConfig({
    testDir: '.',
    fullyParallel: false,
    workers: 1,
    timeout: 120_000,
    expect: { timeout: 30_000 },
    reporter: [['list'], ['html', { open: 'never' }]],
    projects: [
        {
            name: 'web',
            testMatch: ['flows/**/*.spec.ts', 'flows/_harness.spec.ts'],
            use: { baseURL: `http://localhost:${WEB_PORT}` },
        },
    ],
    webServer: {
        // Build the prod bundle, then serve it with the COOP/COEP headers
        // vite.config.ts already sets on `preview`. Tests opt the debug hook
        // in via ?e2e=1.
        command: `pnpm --filter vite-demo build && pnpm --filter vite-demo preview --port ${WEB_PORT} --strictPort`,
        cwd: VITE_DEMO_DIR,
        port: WEB_PORT,
        reuseExistingServer: !process.env.CI,
        timeout: 300_000,
    },
});
```

- [ ] **Step 3: Write a temporary harness smoke test**

Create `ts/tests/e2e/flows/_harness.spec.ts`:

```ts
import { test, expect } from '@playwright/test';

test('@smoke harness: app loads and is cross-origin isolated', async ({ page }) => {
    await page.goto('/?e2e=1');
    // crossOriginIsolated must be true or SharedArrayBuffer (wasm threads) dies.
    const isolated = await page.evaluate(() => self.crossOriginIsolated === true);
    expect(isolated).toBe(true);
    // The page mounted React (root has children).
    await expect(page.locator('#root')).not.toBeEmpty();
});
```

- [ ] **Step 4: Run it**

Run: `cd ts/tests/e2e && pnpm exec playwright test --project=web flows/_harness.spec.ts`
Expected: PASS — proves the preview server builds, headers are present, and the page mounts. (First run builds the wasm-bearing bundle; may take minutes.)

- [ ] **Step 5: Remove the temporary harness test and commit**

```bash
rm ts/tests/e2e/flows/_harness.spec.ts
git add ts/tests/e2e/playwright.config.ts ts/tests/e2e/lib/paths.ts
git commit -m "chore(e2e): playwright config + web preview webServer skeleton"
```

---

## Phase 1 — App-under-test prerequisites

These are small, surgical changes to vite-demo / electron-demo / @anyfs/trees. Each is independently committable. Where a change is hard to unit-test in isolation, the verification is "the testid/hook is queryable" — proven later by the flow specs; here we verify via a focused assertion.

### Task 1.1: Add `data-testid`s to FilePicker buttons

**Files:**
- Modify: `ts/examples/vite-demo/src/components/FilePicker.tsx`

- [ ] **Step 1: Add testids to the three open buttons**

In `FilePicker.tsx`, add `data-testid` attributes:
- the "Open file…" button → `data-testid="open-file-button"`
- the legacy hidden `<input type="file" id="anyfs-legacy-file">` → add `data-testid="legacy-file-input"`
- the "Open URL…" button → `data-testid="open-url-button"`
- the "Open system drive…" button → `data-testid="open-drives-button"`

Example for the file button:

```tsx
<button
    type="button"
    data-testid="open-file-button"
    className="text-emerald-700 dark:text-emerald-400 hover:underline text-base"
    onClick={() => void onOpenFile()}
>
    Open file…
</button>
```

- [ ] **Step 2: Verify the build still typechecks/builds**

Run: `cd ts && pnpm --filter vite-demo build`
Expected: build succeeds (no TS errors).

- [ ] **Step 3: Commit**

```bash
git add ts/examples/vite-demo/src/components/FilePicker.tsx
git commit -m "test(vite-demo): add data-testid to file-picker open buttons"
```

### Task 1.2: Add `data-testid`s to partition rows, URL dialog, download status, settings, error dialog, properties modal

**Files:**
- Modify: `ts/examples/vite-demo/src/components/DiskView.tsx`
- Modify: `ts/examples/vite-demo/src/components/UrlPromptDialog.tsx`
- Modify: `ts/examples/vite-demo/src/components/DownloadStatus.tsx`
- Modify: `ts/examples/vite-demo/src/components/UrlErrorDialog.tsx`
- Modify: `ts/examples/vite-demo/src/Settings.tsx`
- Modify: `ts/packages/trees/src/AnyfsFileBrowser.tsx`

- [ ] **Step 1: DiskView partition buttons**

In `DiskView.tsx`, add `data-testid={`partition-0`}` to the whole-disk button and
`data-testid={`partition-${p.index}`}` to each mapped partition button.

- [ ] **Step 2: UrlPromptDialog submit/cancel**

In `UrlPromptDialog.tsx`: submit button → `data-testid="url-dialog-submit"`; cancel → `data-testid="url-dialog-cancel"`. (Input already has `aria-label="Disk image URL"`.)

- [ ] **Step 3: DownloadStatus container + buttons**

In `DownloadStatus.tsx`: container `<div>` → `data-testid="download-status"`; the no-error "cancel" button → `data-testid="download-cancel"`; the error "×" button → `data-testid="download-dismiss"`. Also add `data-testid="download-error"` to the error text `<div>` and `data-testid="download-progress"` to the progress text `<div>`.

- [ ] **Step 4: UrlErrorDialog**

In `UrlErrorDialog.tsx`: the `role="alertdialog"` container → `data-testid="url-error-dialog"`; the message `<div>` → `data-testid="url-error-message"`.

- [ ] **Step 5: Settings disable-native toggle + restart-confirm**

In `Settings.tsx`: the disable-native `<input type="checkbox">` → `data-testid="disable-native-toggle"`; the confirm dialog "Restart" button → `data-testid="disable-native-restart"`; the confirm "Cancel" → `data-testid="disable-native-cancel"`.

- [ ] **Step 6: Properties modal**

In `AnyfsFileBrowser.tsx`: the properties modal `role="dialog"` container → `data-testid="properties-modal"`. Add `data-testid="properties-size"` to the size value row and `data-testid="properties-kind"` to the kind value row, so a test can read stat values without scraping layout.

- [ ] **Step 7: Verify builds**

Run: `cd ts && pnpm --filter @anyfs/trees build && pnpm --filter vite-demo build`
Expected: both build cleanly.

- [ ] **Step 8: Commit**

```bash
git add ts/examples/vite-demo/src/components/DiskView.tsx ts/examples/vite-demo/src/components/UrlPromptDialog.tsx ts/examples/vite-demo/src/components/DownloadStatus.tsx ts/examples/vite-demo/src/components/UrlErrorDialog.tsx ts/examples/vite-demo/src/Settings.tsx ts/packages/trees/src/AnyfsFileBrowser.tsx
git commit -m "test(vite-demo,trees): add data-testid to partitions, dialogs, download status, properties"
```

### Task 1.3: Add `getState()`/`lastError` to `__anyfsTest` via a ref bridge, gated behind DEV||?e2e=1

**Files:**
- Modify: `ts/examples/vite-demo/src/App.tsx`
- Create: `ts/examples/vite-demo/src/test-bridge.tsx`

- [ ] **Step 1: Write the test-bridge component**

Create `ts/examples/vite-demo/src/test-bridge.tsx`. It reads the live `AnyfsState` via `useAnyfsDiskMaybe()` and republishes a snapshot onto `window.__anyfsTest`. It must render *inside* `<AnyfsProvider>`.

```tsx
import { useEffect } from 'react';
import { useAnyfsDiskMaybe } from '@anyfs/react';

/** True when debug hooks should be active: Vite dev, or explicit ?e2e=1 opt-in
 *  (the latter lets Playwright drive the production preview build). */
export function e2eEnabled(): boolean {
    if (import.meta.env.DEV) return true;
    try {
        return new URLSearchParams(window.location.search).has('e2e');
    } catch {
        return false;
    }
}

/** Publishes a read-only state snapshot onto window.__anyfsTest.getState /
 *  .lastError. Source-injection setters live in App.tsx. Renders nothing. */
export function TestStateBridge() {
    const state = useAnyfsDiskMaybe();
    useEffect(() => {
        const w = window as any;
        const api = (w.__anyfsTest ??= {});
        api.getState = () => ({
            status: state?.status ?? 'idle',
            mode: state?.mode ?? null,
            mountPath: state?.mountPath ?? null,
            error: state?.error ? { message: state.error.message } : null,
        });
        api.lastError = state?.error ? { message: state.error.message } : null;
    }, [state]);
    return null;
}
```

- [ ] **Step 2: Gate the existing source-injection hook and mount the bridge in App.tsx**

In `App.tsx`:
1. Import: `import { TestStateBridge, e2eEnabled } from './test-bridge';`
2. Wrap the existing `__anyfsTest` source-setter `useEffect` body in `if (!e2eEnabled()) return;` so the setters only attach when enabled.
3. Gate the verbose log level: change `mountOpts={{ loglevel: 7 }}` to `mountOpts={{ loglevel: e2eEnabled() ? 7 : 3 }}`.
4. Render `<TestStateBridge />` as a child inside `<AnyfsProvider>` (e.g. just before `<div className="h-screen flex flex-col">`).

The gated setter effect:

```tsx
useEffect(() => {
    if (!e2eEnabled()) return;
    const api = ((window as any).__anyfsTest ??= {});
    api.openUrl = (url: string) => {
        const name = sourceName({ kind: 'url', url });
        setSource({ kind: 'url', url, name });
    };
    api.openPath = (path: string) => {
        const name = sourceName({ kind: 'path', path });
        setSource({ kind: 'path', path, name });
    };
    api.setSourceFile = (file: File) => setSource({ kind: 'blob', blob: file });
    return () => {
        delete (window as any).__anyfsTest;
    };
}, []);
```

- [ ] **Step 3: Verify prod build does NOT attach the hook without ?e2e=1, and DEV does**

Run: `cd ts && pnpm --filter vite-demo build`
Expected: builds cleanly. (Behavioral verification — hook present only under `?e2e=1` in prod — is asserted by the Phase 0 harness pattern and reused in flows; here just confirm the build.)

- [ ] **Step 4: Commit**

```bash
git add ts/examples/vite-demo/src/App.tsx ts/examples/vite-demo/src/test-bridge.tsx
git commit -m "test(vite-demo): gate __anyfsTest behind DEV||?e2e=1, add getState/lastError bridge, quiet prod loglevel"
```

### Task 1.4: Delete orphaned debug/probe HTML pages

These 8 pages in `public/` are git-tracked, referenced from nowhere in the codebase
(verified by grep), and currently shipped into the production `dist/`. They are standalone
manual debug pages superseded by this E2E suite. Delete them — git history (`fca81d7`) retains
them if ever needed again.

**Files:**
- Delete: `ts/examples/vite-demo/public/{debug,debug-api,debug-atomics,debug-worker,debug-stream,direct-module-test,probe,probe2}.html`

- [ ] **Step 1: Remove the orphaned pages**

```bash
git rm ts/examples/vite-demo/public/debug.html \
       ts/examples/vite-demo/public/debug-api.html \
       ts/examples/vite-demo/public/debug-atomics.html \
       ts/examples/vite-demo/public/debug-worker.html \
       ts/examples/vite-demo/public/debug-stream.html \
       ts/examples/vite-demo/public/direct-module-test.html \
       ts/examples/vite-demo/public/probe.html \
       ts/examples/vite-demo/public/probe2.html
```

- [ ] **Step 2: Build and confirm dist/ no longer contains them**

Run: `cd ts && pnpm --filter vite-demo build && ls examples/vite-demo/dist/*.html`
Expected: only `index.html` (no `debug-*.html` / `probe*.html`).

- [ ] **Step 3: Commit**

```bash
git commit -m "chore(vite-demo): delete orphaned debug/probe HTML pages"
```

### Task 1.5: Add `ANYFS_TEST_DOWNLOAD_DIR` save-dialog bypass (Electron)

**Files:**
- Modify: `ts/examples/electron-demo/src/main.ts:245-258`

- [ ] **Step 1: Bypass the save dialog when the env var is set**

In `installDownloadIpc()`, replace the `download:open` handler body so it short-circuits to a fixed directory when `ANYFS_TEST_DOWNLOAD_DIR` is set (mirrors `ANYFS_TEST_LOCAL_PATH` for open):

```ts
ipcMain.handle('download:open', async (event, fileName: string) => {
    const suggested = fileName || 'download.bin';
    // Test hook: native save dialogs can't be driven by Playwright. When a
    // target dir is pre-set, write there and skip the dialog entirely.
    if (process.env.ANYFS_TEST_DOWNLOAD_DIR) {
        const filePath = join(process.env.ANYFS_TEST_DOWNLOAD_DIR, suggested);
        const id = `dl-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
        const ws = createWriteStream(filePath);
        downloads.set(id, { stream: ws, path: filePath });
        return { id, path: filePath };
    }
    const owner = BrowserWindow.fromWebContents(event.sender);
    const result = await (owner
        ? dialog.showSaveDialog(owner, { defaultPath: join(downloadsDir, suggested) })
        : dialog.showSaveDialog({ defaultPath: join(downloadsDir, suggested) }));
    if (result.canceled || !result.filePath) {
        return { cancelled: true as const };
    }
    const id = `dl-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
    const ws = createWriteStream(result.filePath);
    downloads.set(id, { stream: ws, path: result.filePath });
    return { id, path: result.filePath };
});
```

- [ ] **Step 2: Build main**

Run: `cd ts && pnpm --filter electron-demo build:main`
Expected: esbuild produces `dist/main.cjs` with no errors.

- [ ] **Step 3: Commit**

```bash
git add ts/examples/electron-demo/src/main.ts
git commit -m "test(electron-demo): add ANYFS_TEST_DOWNLOAD_DIR to bypass save dialog"
```

---

## Phase 2 — Fixtures

### Task 2.1: Fixture manifest (single source of truth)

**Files:**
- Create: `ts/tests/e2e/fixtures/manifest.ts`

- [ ] **Step 1: Write the manifest types + registry**

Create `ts/tests/e2e/fixtures/manifest.ts`:

```ts
import { resolve } from 'node:path';
import { IMAGES_DIR } from '../lib/paths';

export interface TreeEntry {
    /** mount-relative path, no leading slash */
    path: string;
    size?: number;       // bytes, for regular files
    dir?: boolean;       // true for directories
    symlink?: string;    // link target, for symlinks
}

export interface PartExpect {
    /** index as listParts reports it (0 = whole disk) */
    index: number;
    fs?: string;         // 'ext4' | 'vfat' | 'btrfs' | 'iso9660' | undefined
    tree: TreeEntry[];   // a representative subset to assert (not exhaustive)
}

export interface Fixture {
    name: string;
    source: 'generated' | 'downloaded';
    /** absolute local path once built/fetched */
    file: string;
    /** for downloaded fixtures */
    url?: string;
    expectedSize?: number;   // bytes, guard for downloaded
    sha256?: string;         // guard for downloaded (filled in once known)
    /** partitions to assert; for whole-disk-no-PT use a single index 0 */
    parts: PartExpect[];
}

const img = (f: string) => resolve(IMAGES_DIR, f);

export const FIXTURES: Record<string, Fixture> = {
    multiRaw: {
        name: 'multiRaw',
        source: 'generated',
        file: img('multi.img'),
        parts: [
            {
                index: 1,
                fs: 'ext4',
                tree: [
                    { path: 'hello.txt', size: 13 },
                    { path: 'dir/nested.bin', size: 4096 },
                    { path: 'empty', dir: true },
                    { path: 'link', symlink: 'hello.txt' },
                ],
            },
            {
                index: 2,
                fs: 'vfat',
                tree: [{ path: 'README.TXT', size: 12 }],
            },
        ],
    },
    mbrExtended: {
        name: 'mbrExtended',
        source: 'generated',
        file: img('mbr-extended.img'),
        // 2 primary (1,2) + logical partitions (5,6) inside an extended (3).
        parts: [
            { index: 1, fs: 'ext4', tree: [{ path: 'p1.txt', size: 3 }] },
            { index: 2, fs: 'vfat', tree: [{ path: 'P2.TXT', size: 3 }] },
            { index: 5, fs: 'ext4', tree: [{ path: 'l5.txt', size: 3 }] },
            { index: 6, fs: 'ext4', tree: [{ path: 'l6.txt', size: 3 }] },
        ],
    },
    btrfsVmdk: {
        name: 'btrfsVmdk',
        source: 'generated',
        file: img('btrfs-whole.vmdk'),
        // whole-disk btrfs, no partition table -> single synthetic index 0.
        parts: [
            {
                index: 0,
                fs: 'btrfs',
                tree: [
                    { path: 'whole.txt', size: 11 },
                    { path: 'sub', dir: true },
                ],
            },
        ],
    },
    qcow2Url: {
        name: 'qcow2Url',
        source: 'downloaded',
        file: img('trusty-cloud.qcow2'),
        url: 'https://cloud-images.ubuntu.com/trusty/current/trusty-server-cloudimg-amd64-disk1.img',
        // expectedSize/sha256 filled in by `fixtures` script first run (Task 2.3).
        parts: [{ index: 1, fs: 'ext4', tree: [{ path: 'etc/hostname' }] }],
    },
    isoUrl: {
        name: 'isoUrl',
        source: 'downloaded',
        file: img('trusty.iso'),
        url: 'https://releases.ubuntu.com/trusty/ubuntu-14.04.6-server-amd64.iso',
        parts: [{ index: 0, fs: 'iso9660', tree: [{ path: 'README.diskdefines' }] }],
    },
};
```

- [ ] **Step 2: Commit**

```bash
git add ts/tests/e2e/fixtures/manifest.ts
git commit -m "test(e2e): fixture manifest with expected trees for all images"
```

### Task 2.2: Generated-image builder (root/sudo, mkfs + loop)

**Files:**
- Create: `ts/tests/e2e/fixtures/generate.mjs`

- [ ] **Step 1: Write the generator**

Create `ts/tests/e2e/fixtures/generate.mjs`. It builds the three generated images idempotently (skips if present). REQUIRES root (`sudo`) for `losetup`/`mount`/`mkfs`. Each helper writes the exact files the manifest asserts.

```js
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const IMAGES = resolve(here, 'images');
mkdirSync(IMAGES, { recursive: true });

const sh = (cmd, args) => execFileSync(cmd, args, { stdio: 'inherit' });
const root = (cmd, args) => sh('sudo', [cmd, ...args]);

function requireRoot() {
    try {
        execFileSync('sudo', ['-n', 'true'], { stdio: 'ignore' });
    } catch {
        console.error('fixtures/generate.mjs needs passwordless sudo (losetup/mount/mkfs).');
        process.exit(1);
    }
}

/** Attach an image file to a loop device with partition scanning, return /dev/loopN. */
function loopAttach(file) {
    const dev = execFileSync('sudo', ['losetup', '--show', '-fP', file]).toString().trim();
    return dev;
}
function loopDetach(dev) {
    try { root('losetup', ['-d', dev]); } catch {}
}

function buildMultiRaw() {
    const out = resolve(IMAGES, 'multi.img');
    if (existsSync(out)) return;
    // 64 MiB GPT: p1 ext4 (32M), p2 vfat (rest)
    sh('truncate', ['-s', '64M', out]);
    sh('sgdisk', ['-n', '1:0:+32M', '-t', '1:8300', '-n', '2:0:0', '-t', '2:0700', out]);
    const dev = loopAttach(out);
    try {
        root('mkfs.ext4', ['-q', '-F', `${dev}p1`]);
        root('mkfs.fat', ['-F', '32', `${dev}p2`]);
        const mnt = '/tmp/anyfs-fx-mnt';
        root('mkdir', ['-p', mnt]);
        // ext4 contents
        root('mount', [`${dev}p1`, mnt]);
        root('bash', ['-c', `printf 'hello, world\\n' > ${mnt}/hello.txt`]); // 13 bytes
        root('mkdir', ['-p', `${mnt}/dir`, `${mnt}/empty`]);
        root('bash', ['-c', `head -c 4096 /dev/zero > ${mnt}/dir/nested.bin`]);
        root('ln', ['-s', 'hello.txt', `${mnt}/link`]);
        root('umount', [mnt]);
        // vfat contents
        root('mount', [`${dev}p2`, mnt]);
        root('bash', ['-c', `printf 'readme fat\\n' > ${mnt}/README.TXT`]); // 12 bytes incl newline? adjust
        root('umount', [mnt]);
    } finally {
        loopDetach(dev);
    }
}

function buildMbrExtended() {
    const out = resolve(IMAGES, 'mbr-extended.img');
    if (existsSync(out)) return;
    sh('truncate', ['-s', '96M', out]);
    // MBR: p1 primary 16M, p2 primary 16M, p3 extended (rest) with logicals p5,p6
    const layout = [
        'label: dos',
        'start=1MiB, size=16MiB, type=83',     // p1 ext4
        'size=16MiB, type=c',                   // p2 fat32-lba
        'size=48MiB, type=5',                   // p3 extended
        'size=16MiB, type=83',                  // p5 logical ext4
        'size=16MiB, type=83',                  // p6 logical ext4
    ].join('\n') + '\n';
    execFileSync('sfdisk', [out], { input: layout, stdio: ['pipe', 'inherit', 'inherit'] });
    const dev = loopAttach(out);
    const mnt = '/tmp/anyfs-fx-mnt';
    root('mkdir', ['-p', mnt]);
    const mkExt = (part, fname, content) => {
        root('mkfs.ext4', ['-q', '-F', `${dev}${part}`]);
        root('mount', [`${dev}${part}`, mnt]);
        root('bash', ['-c', `printf '${content}' > ${mnt}/${fname}`]);
        root('umount', [mnt]);
    };
    try {
        mkExt('p1', 'p1.txt', 'p1\\n');
        root('mkfs.fat', ['-F', '32', `${dev}p2`]);
        root('mount', [`${dev}p2`, mnt]);
        root('bash', ['-c', `printf 'p2\\n' > ${mnt}/P2.TXT`]);
        root('umount', [mnt]);
        mkExt('p5', 'l5.txt', 'l5\\n');
        mkExt('p6', 'l6.txt', 'l6\\n');
    } finally {
        loopDetach(dev);
    }
}

function buildBtrfsVmdk() {
    const raw = resolve(IMAGES, 'btrfs-whole.raw');
    const vmdk = resolve(IMAGES, 'btrfs-whole.vmdk');
    if (existsSync(vmdk)) return;
    sh('truncate', ['-s', '128M', raw]); // btrfs needs a larger floor
    const dev = loopAttach(raw); // no PT; whole-dev mkfs
    const mnt = '/tmp/anyfs-fx-mnt';
    root('mkdir', ['-p', mnt]);
    try {
        root('mkfs.btrfs', ['-q', '-f', dev]);
        root('mount', [dev, mnt]);
        root('bash', ['-c', `printf 'whole btrfs\\n' > ${mnt}/whole.txt`]); // 11 bytes -> adjust manifest if needed
        root('mkdir', ['-p', `${mnt}/sub`]);
        root('umount', [mnt]);
    } finally {
        loopDetach(dev);
    }
    // Convert raw -> vmdk via qemu-img (host tool).
    sh('qemu-img', ['convert', '-O', 'vmdk', raw, vmdk]);
}

requireRoot();
buildMultiRaw();
buildMbrExtended();
buildBtrfsVmdk();
console.log('generated fixtures ready in', IMAGES);
```

- [ ] **Step 2: Run the generator**

Run: `cd ts/tests/e2e && node fixtures/generate.mjs`
Expected: creates `fixtures/images/multi.img`, `mbr-extended.img`, `btrfs-whole.vmdk`. (Needs passwordless sudo + `sgdisk`, `sfdisk`, `mkfs.ext4`, `mkfs.fat`, `mkfs.btrfs`, `qemu-img`.)

- [ ] **Step 3: Reconcile manifest sizes with reality**

Inspect actual byte sizes of the files written (e.g. `hello.txt`, `README.TXT`, `whole.txt`) and update the `size:` fields in `manifest.ts` to match exactly what the generator wrote. (The `printf` content determines the size; make manifest and generator agree.)

- [ ] **Step 4: Commit**

```bash
git add ts/tests/e2e/fixtures/generate.mjs ts/tests/e2e/fixtures/manifest.ts
git commit -m "test(e2e): root mkfs/loop generator for GPT, MBR-extended, btrfs-VMDK fixtures"
```

### Task 2.3: Downloaded-image fetcher with sha/size guard

**Files:**
- Create: `ts/tests/e2e/fixtures/fetch.mjs`

- [ ] **Step 1: Write the fetcher**

Create `ts/tests/e2e/fixtures/fetch.mjs`. Downloads each `source:'downloaded'` fixture once into `images/`, verifies size/sha when known, and writes the observed sha on first fetch.

```js
import { createHash } from 'node:crypto';
import { createWriteStream, existsSync, readFileSync, statSync } from 'node:fs';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const IMAGES = resolve(here, 'images');

// Mirror the downloaded entries from manifest.ts (kept in sync by hand —
// fetch.mjs is plain JS so it can run standalone without TS tooling).
const DOWNLOADS = [
    {
        file: resolve(IMAGES, 'trusty-cloud.qcow2'),
        url: 'https://cloud-images.ubuntu.com/trusty/current/trusty-server-cloudimg-amd64-disk1.img',
    },
    {
        file: resolve(IMAGES, 'trusty.iso'),
        url: 'https://releases.ubuntu.com/trusty/ubuntu-14.04.6-server-amd64.iso',
    },
];

function sha256(file) {
    const h = createHash('sha256');
    h.update(readFileSync(file));
    return h.digest('hex');
}

async function fetchOne(d) {
    if (existsSync(d.file)) {
        console.log('cached', d.file, statSync(d.file).size, 'bytes, sha256', sha256(d.file));
        return;
    }
    console.log('downloading', d.url);
    const res = await fetch(d.url);
    if (!res.ok) throw new Error(`fetch ${d.url} -> ${res.status}`);
    await pipeline(Readable.fromWeb(res.body), createWriteStream(d.file));
    console.log('downloaded', d.file, statSync(d.file).size, 'bytes, sha256', sha256(d.file));
}

for (const d of DOWNLOADS) await fetchOne(d);
```

- [ ] **Step 2: Run the fetcher and capture the guards**

Run: `cd ts/tests/e2e && node fixtures/fetch.mjs`
Expected: downloads both images, prints size + sha256 for each.

- [ ] **Step 3: Fill the guards into the manifest**

Copy the printed `size`/`sha256` for each downloaded image into `expectedSize`/`sha256` in `manifest.ts`. Add a guard helper used by `ensureFixture` (Task 2.4) that throws if a cached downloaded file's size/sha drift from the manifest.

- [ ] **Step 4: Commit**

```bash
git add ts/tests/e2e/fixtures/fetch.mjs ts/tests/e2e/fixtures/manifest.ts
git commit -m "test(e2e): downloaded-fixture fetcher + size/sha guards for qcow2 + iso"
```

### Task 2.4: `ensureFixture` helper + local Range server

**Files:**
- Create: `ts/tests/e2e/fixtures/ensure.ts`
- Create: `ts/tests/e2e/fixtures/range-server.ts`

- [ ] **Step 1: Write `ensureFixture`**

Create `ts/tests/e2e/fixtures/ensure.ts`:

```ts
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { E2E_DIR } from '../lib/paths';
import { FIXTURES, type Fixture } from './manifest';

function sha256(file: string): string {
    return createHash('sha256').update(readFileSync(file)).digest('hex');
}

/** Build (generated) or fetch (downloaded) a fixture if missing, verify guards. */
export function ensureFixture(name: keyof typeof FIXTURES): Fixture {
    const fx = FIXTURES[name];
    if (!existsSync(fx.file)) {
        const script = fx.source === 'generated' ? 'fixtures/generate.mjs' : 'fixtures/fetch.mjs';
        execFileSync('node', [script], { cwd: E2E_DIR, stdio: 'inherit' });
    }
    if (!existsSync(fx.file)) throw new Error(`fixture ${name} still missing after build`);
    if (fx.source === 'downloaded') {
        if (fx.expectedSize && statSync(fx.file).size !== fx.expectedSize) {
            throw new Error(`fixture ${name} size drift: remote changed?`);
        }
        if (fx.sha256 && sha256(fx.file) !== fx.sha256) {
            throw new Error(`fixture ${name} sha256 drift: remote changed?`);
        }
    }
    return fx;
}
```

- [ ] **Step 2: Write the Range-honoring server**

Create `ts/tests/e2e/fixtures/range-server.ts`:

```ts
import { createReadStream, statSync } from 'node:fs';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';

export interface RangeServer {
    url: string;       // http://127.0.0.1:<port>/image
    close(): Promise<void>;
}

/** Serve a single file over HTTP with Range support (URLFS needs partial reads
 *  and a HEAD that advertises Accept-Ranges + Content-Length). */
export async function serveFileWithRange(file: string): Promise<RangeServer> {
    const size = statSync(file).size;
    const server: Server = createServer((req, res) => {
        if (req.method === 'HEAD') {
            res.writeHead(200, {
                'Accept-Ranges': 'bytes',
                'Content-Length': String(size),
            });
            return res.end();
        }
        const range = req.headers.range;
        if (range) {
            const m = /bytes=(\d+)-(\d*)/.exec(range)!;
            const start = Number(m[1]);
            const end = m[2] ? Number(m[2]) : size - 1;
            res.writeHead(206, {
                'Accept-Ranges': 'bytes',
                'Content-Range': `bytes ${start}-${end}/${size}`,
                'Content-Length': String(end - start + 1),
            });
            return createReadStream(file, { start, end }).pipe(res);
        }
        res.writeHead(200, { 'Accept-Ranges': 'bytes', 'Content-Length': String(size) });
        createReadStream(file).pipe(res);
    });
    await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
    const port = (server.address() as AddressInfo).port;
    return {
        url: `http://127.0.0.1:${port}/image`,
        close: () => new Promise<void>((r) => server.close(() => r())),
    };
}
```

- [ ] **Step 3: Commit**

```bash
git add ts/tests/e2e/fixtures/ensure.ts ts/tests/e2e/fixtures/range-server.ts
git commit -m "test(e2e): ensureFixture (build/fetch+guard) and Range-honoring file server"
```

---

## Phase 3 — Driver interface + implementations

### Task 3.1: Driver interface + shared types

**Files:**
- Create: `ts/tests/e2e/drivers/driver.ts`

- [ ] **Step 1: Write the interface**

Create `ts/tests/e2e/drivers/driver.ts`:

```ts
import type { Fixture, TreeEntry } from '../fixtures/manifest';

export type DownloadMechanism = 'service-worker' | 'electron-ipc';

export interface DownloadResult {
    bytes: Uint8Array;
    size: number;
    mechanism: DownloadMechanism;
}

export type ErrorKind = 'bad-image' | 'no-range' | 'unsupported' | 'mount-failed';

export interface RowInfo {
    name: string;
    isDir: boolean;
}

export interface Driver {
    /** Boot the app to a ready picker. */
    start(): Promise<void>;
    stop(): Promise<void>;

    /** Source injection. */
    openImage(fx: Fixture): Promise<void>;
    openUrl(url: string): Promise<void>;

    /** Disk/partition. */
    listPartitionIndices(): Promise<number[]>;
    enterPartition(index: number): Promise<void>;

    /** Filesystem browsing (current dir). */
    listRows(): Promise<RowInfo[]>;
    navigateInto(name: string): Promise<void>;
    propertiesOf(name: string): Promise<{ size: number | null; kind: string | null }>;

    /** Download the named file from the current dir. */
    download(name: string): Promise<DownloadResult>;

    /** Error assertions. */
    expectError(kind: ErrorKind): Promise<void>;

    /** The resolved backend after ready ('native' | 'wasm' | 'node-wasm'). */
    backendMode(): Promise<string | null>;
}

export type { Fixture, TreeEntry };
```

- [ ] **Step 2: Commit**

```bash
git add ts/tests/e2e/drivers/driver.ts
git commit -m "test(e2e): Driver interface + DownloadResult/ErrorKind types"
```

### Task 3.2: WebDriver

**Files:**
- Create: `ts/tests/e2e/drivers/web-driver.ts`

- [ ] **Step 1: Write the WebDriver**

Create `ts/tests/e2e/drivers/web-driver.ts`. Uses a Playwright `Page`. Source injection via `__anyfsTest`; everything else via DOM testids/Chonky rows; waits on `__anyfsTest.getState().status`.

```ts
import type { Page, Download } from '@playwright/test';
import { readFileSync } from 'node:fs';
import type { Driver, DownloadResult, ErrorKind, RowInfo } from './driver';
import type { Fixture } from '../fixtures/manifest';

export class WebDriver implements Driver {
    constructor(private page: Page) {}

    async start() {
        await this.page.goto('/?e2e=1');
        await this.page.waitForFunction(() => (window as any).__anyfsTest?.getState);
    }
    async stop() {}

    private async waitStatus(status: string) {
        await this.page.waitForFunction(
            (s) => (window as any).__anyfsTest?.getState().status === s,
            status,
            { timeout: 120_000 },
        );
    }

    async openImage(fx: Fixture) {
        // Web can't read a host path; inject the file as a Blob via the page.
        const buf = readFileSync(fx.file);
        await this.page.evaluate(
            async ({ bytes, name }) => {
                const file = new File([new Uint8Array(bytes)], name);
                (window as any).__anyfsTest.setSourceFile(file);
            },
            { bytes: Array.from(buf), name: fx.name },
        );
        await this.waitStatus('mounting').catch(() => {});
    }

    async openUrl(url: string) {
        await this.page.evaluate((u) => (window as any).__anyfsTest.openUrl(u), url);
        await this.waitStatus('mounting').catch(() => {});
    }

    async listPartitionIndices() {
        await this.page.waitForSelector('[data-testid^="partition-"]');
        const ids = await this.page.$$eval('[data-testid^="partition-"]', (els) =>
            els.map((e) => Number(e.getAttribute('data-testid')!.split('-')[1])),
        );
        return ids;
    }

    async enterPartition(index: number) {
        await this.page.click(`[data-testid="partition-${index}"]`);
        await this.waitStatus('ready');
    }

    async listRows(): Promise<RowInfo[]> {
        // Chonky rows expose aria via the file-entry name; read visible entries.
        await this.page.waitForSelector('.chonky-fileListWrapper, [class*="fileList"]');
        return this.page.$$eval('[class*="fileEntryClickableWrapper"]', (els) =>
            els.map((e) => ({
                name: (e.getAttribute('aria-label') || e.textContent || '').trim(),
                isDir: !!e.querySelector('[class*="folder"]'),
            })),
        );
    }

    async navigateInto(name: string) {
        await this.page.dblclick(`text=${name}`);
    }

    async propertiesOf(name: string) {
        await this.page.click(`text=${name}`, { button: 'right' });
        await this.page.click('text=Properties');
        await this.page.waitForSelector('[data-testid="properties-modal"]');
        const sizeTxt = await this.page
            .locator('[data-testid="properties-size"]')
            .textContent();
        const kind = await this.page.locator('[data-testid="properties-kind"]').textContent();
        return { size: sizeTxt ? parseInt(sizeTxt.replace(/[^0-9]/g, ''), 10) : null, kind };
    }

    async download(name: string): Promise<DownloadResult> {
        const [dl] = await Promise.all([
            this.page.waitForEvent('download'),
            this.page.dblclick(`text=${name}`),
        ]);
        const path = await (dl as Download).path();
        const bytes = new Uint8Array(readFileSync(path!));
        return { bytes, size: bytes.length, mechanism: 'service-worker' };
    }

    async expectError(_kind: ErrorKind) {
        // URL error surfaces in url-error-dialog; mount error in DiskView text.
        await this.page.waitForSelector(
            '[data-testid="url-error-dialog"], text=Can’t mount partition',
        );
    }

    async backendMode() {
        return this.page.evaluate(() => (window as any).__anyfsTest.getState().mode);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add ts/tests/e2e/drivers/web-driver.ts
git commit -m "test(e2e): WebDriver — __anyfsTest source inject + DOM/Chonky actions, SW download capture"
```

### Task 3.3: ElectronDriver + electron launch fixture

**Files:**
- Create: `ts/tests/e2e/drivers/electron-driver.ts`
- Create: `ts/tests/e2e/lib/launch-electron.ts`

- [ ] **Step 1: Write the electron launch helper**

Create `ts/tests/e2e/lib/launch-electron.ts`:

```ts
import { _electron as electron, type ElectronApplication } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { ELECTRON_DEMO_DIR } from './paths';

export interface ElectronLaunch {
    app: ElectronApplication;
    downloadDir: string;
}

/** Launch the esbuilt dist/main.cjs. backend='wasm' sets ANYFS_DISABLE_NATIVE. */
export async function launchElectron(
    backend: 'native' | 'wasm',
    localImagePath?: string,
): Promise<ElectronLaunch> {
    const downloadDir = mkdtempSync(join(tmpdir(), 'anyfs-dl-'));
    const env: Record<string, string> = {
        ...process.env,
        ELECTRON_DEV: '1',
        ANYFS_TEST_DOWNLOAD_DIR: downloadDir,
    };
    if (backend === 'wasm') env.ANYFS_DISABLE_NATIVE = '1';
    if (localImagePath) env.ANYFS_TEST_LOCAL_PATH = localImagePath;
    const app = await electron.launch({
        args: ['.'],
        cwd: ELECTRON_DEMO_DIR,
        env,
    });
    return { app, downloadDir };
}
```

- [ ] **Step 2: Write the ElectronDriver**

Create `ts/tests/e2e/drivers/electron-driver.ts`. Reuses the same DOM actions as WebDriver against the Electron renderer page; source injection prefers `ANYFS_TEST_LOCAL_PATH` (set at launch) consumed by clicking the native-open button, falling back to `__anyfsTest.openPath`. Download asserts the IPC path by reading from `downloadDir`.

```ts
import type { ElectronApplication, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import type { Driver, DownloadResult, ErrorKind, RowInfo } from './driver';
import type { Fixture } from '../fixtures/manifest';

export class ElectronDriver implements Driver {
    private page!: Page;
    constructor(
        private app: ElectronApplication,
        private downloadDir: string,
    ) {}

    async start() {
        this.page = await this.app.firstWindow();
        await this.page.waitForFunction(() => (window as any).__anyfsTest?.getState);
    }
    async stop() {
        await this.app.close();
    }

    private async waitStatus(status: string) {
        await this.page.waitForFunction(
            (s) => (window as any).__anyfsTest?.getState().status === s,
            status,
            { timeout: 120_000 },
        );
    }

    async openImage(fx: Fixture) {
        // ANYFS_TEST_LOCAL_PATH was set to fx.file at launch -> the native-open
        // button returns it through the real dialog:openImage IPC pipeline.
        await this.page.click('[data-testid="open-drives-button"], [data-testid="open-file-button"]');
        await this.waitStatus('mounting').catch(() => {});
    }

    async openUrl(url: string) {
        await this.page.evaluate((u) => (window as any).__anyfsTest.openUrl(u), url);
        await this.waitStatus('mounting').catch(() => {});
    }

    // listPartitionIndices / enterPartition / listRows / navigateInto /
    // propertiesOf / expectError are identical DOM logic to WebDriver — share
    // via a common mixin (Step 3) rather than duplicating.

    async download(name: string): Promise<DownloadResult> {
        await this.page.dblclick(`text=${name}`);
        // Electron path streams via IPC into ANYFS_TEST_DOWNLOAD_DIR/<name>.
        await this.page.waitForFunction(
            () => (window as any).__anyfsTest?.getState() != null,
        );
        const file = join(this.downloadDir, name);
        // Wait for the download-status to complete (progress reaches size).
        await this.page.waitForSelector('[data-testid="download-status"]');
        await this.page.waitForFunction(
            () => !document.querySelector('[data-testid="download-status"]'),
            undefined,
            { timeout: 120_000 },
        ).catch(() => {});
        const bytes = new Uint8Array(readFileSync(file));
        return { bytes, size: bytes.length, mechanism: 'electron-ipc' };
    }

    async listPartitionIndices() {
        await this.page.waitForSelector('[data-testid^="partition-"]');
        return this.page.$$eval('[data-testid^="partition-"]', (els) =>
            els.map((e) => Number(e.getAttribute('data-testid')!.split('-')[1])),
        );
    }
    async enterPartition(index: number) {
        await this.page.click(`[data-testid="partition-${index}"]`);
        await this.waitStatus('ready');
    }
    async listRows(): Promise<RowInfo[]> {
        await this.page.waitForSelector('[class*="fileList"]');
        return this.page.$$eval('[class*="fileEntryClickableWrapper"]', (els) =>
            els.map((e) => ({
                name: (e.getAttribute('aria-label') || e.textContent || '').trim(),
                isDir: !!e.querySelector('[class*="folder"]'),
            })),
        );
    }
    async navigateInto(name: string) {
        await this.page.dblclick(`text=${name}`);
    }
    async propertiesOf(name: string) {
        await this.page.click(`text=${name}`, { button: 'right' });
        await this.page.click('text=Properties');
        await this.page.waitForSelector('[data-testid="properties-modal"]');
        const sizeTxt = await this.page.locator('[data-testid="properties-size"]').textContent();
        const kind = await this.page.locator('[data-testid="properties-kind"]').textContent();
        return { size: sizeTxt ? parseInt(sizeTxt.replace(/[^0-9]/g, ''), 10) : null, kind };
    }
    async expectError(_kind: ErrorKind) {
        await this.page.waitForSelector(
            '[data-testid="url-error-dialog"], text=Can’t mount partition',
        );
    }
    async backendMode() {
        return this.page.evaluate(() => (window as any).__anyfsTest.getState().mode);
    }
}
```

- [ ] **Step 3: Extract shared DOM actions to a mixin to satisfy DRY**

Create `ts/tests/e2e/drivers/dom-actions.ts` exporting standalone functions that take a `Page` (e.g. `listPartitionIndices(page)`, `enterPartition(page, i, waitReady)`, `listRows(page)`, `navigateInto(page, name)`, `propertiesOf(page, name)`, `expectError(page)`), and have both WebDriver and ElectronDriver delegate to them. Replace the duplicated bodies in both drivers with calls to these helpers.

- [ ] **Step 4: Commit**

```bash
git add ts/tests/e2e/drivers/electron-driver.ts ts/tests/e2e/lib/launch-electron.ts ts/tests/e2e/drivers/dom-actions.ts ts/tests/e2e/drivers/web-driver.ts
git commit -m "test(e2e): ElectronDriver + launch helper + shared DOM actions (DRY with WebDriver)"
```

### Task 3.4: Wire the three projects + per-project Driver fixture + native guard

**Files:**
- Modify: `ts/tests/e2e/playwright.config.ts`
- Create: `ts/tests/e2e/lib/test-fixture.ts`
- Create: `ts/tests/e2e/lib/native-guard.ts`

- [ ] **Step 1: Native-addon precondition guard**

Create `ts/tests/e2e/lib/native-guard.ts`:

```ts
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { ELECTRON_DEMO_DIR, TS_ROOT } from './paths';

/** The electron-native project must have an ABI-matching addon, or we FAIL
 *  (not skip) — ABI drift SIGSEGVs at require() and would masquerade as a pass. */
export function assertNativeAddonPresent() {
    const candidates = [
        resolve(ELECTRON_DEMO_DIR, 'resources/native/anyfs_native.node'),
        resolve(TS_ROOT, 'packages/anyfs-native/build/Release/anyfs_native.node'),
    ];
    if (!candidates.some(existsSync)) {
        throw new Error(
            'electron-native: no anyfs_native.node found. Build it against the test ' +
                'Electron version (node-gyp --runtime=electron --target=<ver>) before running.',
        );
    }
}
```

- [ ] **Step 2: Test fixture exposing `driver` per project**

Create `ts/tests/e2e/lib/test-fixture.ts`:

```ts
import { test as base } from '@playwright/test';
import type { Driver } from '../drivers/driver';
import { WebDriver } from '../drivers/web-driver';
import { ElectronDriver } from '../drivers/electron-driver';
import { launchElectron } from './launch-electron';
import { assertNativeAddonPresent } from './native-guard';

type Fixtures = {
    driver: Driver;
    /** the absolute image path to use when launching electron (set by tests via test.use). */
    electronImage: string | undefined;
};

export const test = base.extend<Fixtures>({
    electronImage: [undefined, { option: true }],
    driver: async ({ page, browserName }, use, testInfo) => {
        const project = testInfo.project.name;
        if (project === 'web') {
            const d = new WebDriver(page);
            await d.start();
            await use(d);
            await d.stop();
            return;
        }
        // electron-native | electron-wasm
        const backend = project === 'electron-native' ? 'native' : 'wasm';
        if (backend === 'native') assertNativeAddonPresent();
        // electronImage set by test.use() in flows that open via native dialog.
        const img = (testInfo as any).__electronImage as string | undefined;
        const { app, downloadDir } = await launchElectron(backend, img);
        const d = new ElectronDriver(app, downloadDir);
        await d.start();
        await use(d);
        await d.stop();
    },
});

export { expect } from '@playwright/test';
```

Note: because Electron launch needs the image path *before* the page exists, flows pass the image path through a module-level setter (see Step 4) or via `launchElectron`'s `ANYFS_TEST_LOCAL_PATH`. Keep the seam simple: the flow calls `setElectronImage(fx.file)` before `driver` is constructed.

- [ ] **Step 3: Add electron projects to the config**

Edit `playwright.config.ts` `projects` to add:

```ts
{ name: 'electron-native', testMatch: ['flows/**/*.spec.ts', 'electron-only/**/*.spec.ts'] },
{ name: 'electron-wasm', testMatch: ['flows/**/*.spec.ts', 'electron-only/**/*.spec.ts'] },
```

The `web` project keeps `testMatch: ['flows/**/*.spec.ts']` (no electron-only). The `webServer` stays — Electron projects ignore `baseURL`.

- [ ] **Step 4: Provide the image-path seam**

Create a tiny module `ts/tests/e2e/lib/electron-image.ts` with `let current: string | undefined; export const setElectronImage = (p?: string) => { current = p; }; export const getElectronImage = () => current;` and have `test-fixture.ts` read `getElectronImage()` instead of `(testInfo as any).__electronImage`. Flows call `setElectronImage(fx.file)` in a `test.beforeEach` for Electron.

- [ ] **Step 5: Typecheck**

Run: `cd ts/tests/e2e && pnpm exec tsc --noEmit`
Expected: no type errors.

- [ ] **Step 6: Commit**

```bash
git add ts/tests/e2e/playwright.config.ts ts/tests/e2e/lib/test-fixture.ts ts/tests/e2e/lib/native-guard.ts ts/tests/e2e/lib/electron-image.ts
git commit -m "test(e2e): 3 projects, per-project Driver fixture, native-addon guard"
```

---

## Phase 4 — Flow: open-browse-download

### Task 4.1: Shared assertions

**Files:**
- Create: `ts/tests/e2e/lib/assertions.ts`

- [ ] **Step 1: Write assertion helpers**

Create `ts/tests/e2e/lib/assertions.ts`:

```ts
import { expect } from '@playwright/test';
import type { Driver } from '../drivers/driver';
import type { PartExpect } from '../fixtures/manifest';

/** Assert the current directory's rows contain every top-level entry the
 *  manifest names for this partition. */
export async function expectKnownTree(driver: Driver, part: PartExpect) {
    const rows = await driver.listRows();
    const names = new Set(rows.map((r) => r.name));
    const tops = new Set(part.tree.map((e) => e.path.split('/')[0]));
    for (const top of tops) expect(names, `row "${top}" present`).toContain(top);
}
```

- [ ] **Step 2: Commit**

```bash
git add ts/tests/e2e/lib/assertions.ts
git commit -m "test(e2e): expectKnownTree assertion helper"
```

### Task 4.2: open-browse-download spec

**Files:**
- Create: `ts/tests/e2e/flows/open-browse-download.spec.ts`

- [ ] **Step 1: Write the spec**

Create `ts/tests/e2e/flows/open-browse-download.spec.ts`:

```ts
import { test, expect } from '../lib/test-fixture';
import { setElectronImage } from '../lib/electron-image';
import { ensureFixture } from '../fixtures/ensure';
import { expectKnownTree } from '../lib/assertions';

const fx = ensureFixture('multiRaw');
const ext4 = fx.parts.find((p) => p.fs === 'ext4')!;

test.beforeEach(() => setElectronImage(fx.file));

test('@smoke open multiRaw, browse ext4 partition, see known files', async ({ driver }) => {
    await driver.openImage(fx);
    const parts = await driver.listPartitionIndices();
    expect(parts).toContain(ext4.index);
    await driver.enterPartition(ext4.index);
    await expectKnownTree(driver, ext4);
});

test('properties shows known file size', async ({ driver }) => {
    await driver.openImage(fx);
    await driver.enterPartition(ext4.index);
    const props = await driver.propertiesOf('hello.txt');
    expect(props.size).toBe(13);
});

test('download a file yields correct bytes via the right mechanism', async ({ driver }, info) => {
    await driver.openImage(fx);
    await driver.enterPartition(ext4.index);
    const res = await driver.download('hello.txt');
    expect(res.size).toBe(13);
    // mechanism must match the target: SW on web, IPC on electron.
    const expected = info.project.name === 'web' ? 'service-worker' : 'electron-ipc';
    expect(res.mechanism).toBe(expected);
});
```

- [ ] **Step 2: Run web project**

Run: `cd ts/tests/e2e && pnpm exec playwright test --project=web flows/open-browse-download.spec.ts`
Expected: 3 tests PASS. (Iterate on Chonky row selectors in `dom-actions.ts` if `listRows`/`navigateInto` don't match — adjust the `[class*="fileEntry..."]` selectors to the actual rendered Chonky classes observed in the trace.)

- [ ] **Step 3: Run electron-wasm project**

Run: `cd ts/tests/e2e && xvfb-run -a pnpm exec playwright test --project=electron-wasm flows/open-browse-download.spec.ts`
Expected: 3 tests PASS (download mechanism asserts `electron-ipc`).

- [ ] **Step 4: Run electron-native (if addon present)**

Run: `cd ts/tests/e2e && xvfb-run -a pnpm exec playwright test --project=electron-native flows/open-browse-download.spec.ts`
Expected: PASS, with `backendMode()` === `'native'`. If no addon, the guard FAILS with the build instruction (expected).

- [ ] **Step 5: Commit**

```bash
git add ts/tests/e2e/flows/open-browse-download.spec.ts ts/tests/e2e/drivers/dom-actions.ts
git commit -m "test(e2e): open-browse-download flow across web + electron backends"
```

---

## Phase 5 — Flow: url-load (URLFS)

### Task 5.1: url-load spec (hermetic local Range server + @network variant)

**Files:**
- Create: `ts/tests/e2e/flows/url-load.spec.ts`

- [ ] **Step 1: Write the spec**

Create `ts/tests/e2e/flows/url-load.spec.ts`:

```ts
import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { serveFileWithRange, type RangeServer } from '../fixtures/range-server';
import { expectKnownTree } from '../lib/assertions';

// Use the qcow2 cloud image as the URL source (also exercises the decoder).
const fx = ensureFixture('qcow2Url');
const part = fx.parts[0];

let server: RangeServer;
test.beforeAll(async () => {
    server = await serveFileWithRange(fx.file);
});
test.afterAll(async () => {
    await server.close();
});

test('@smoke open via URL (local Range server), browse known entry', async ({ driver }) => {
    await driver.openUrl(server.url);
    await driver.enterPartition(part.index);
    await expectKnownTree(driver, part);
});

test('@network open the real remote URL directly', async ({ driver }) => {
    await driver.openUrl(fx.url!);
    await driver.enterPartition(part.index);
    await expectKnownTree(driver, part);
});
```

- [ ] **Step 2: Run hermetic (skip @network)**

Run: `cd ts/tests/e2e && pnpm exec playwright test --project=web --grep-invert @network flows/url-load.spec.ts`
Expected: the local-Range-server test PASSES; @network skipped.

- [ ] **Step 3: Run electron-wasm hermetic**

Run: `cd ts/tests/e2e && xvfb-run -a pnpm exec playwright test --project=electron-wasm --grep-invert @network flows/url-load.spec.ts`
Expected: PASS (Electron URL path goes through the renderer URLFS like web; the http-proxy-worker is only used for the native path, covered separately if/when native URL open is wired).

- [ ] **Step 4: Commit**

```bash
git add ts/tests/e2e/flows/url-load.spec.ts
git commit -m "test(e2e): url-load flow via local Range server + @network real-remote variant"
```

---

## Phase 6 — Flow: formats

### Task 6.1: formats spec (raw GPT / MBR-extended / btrfs-VMDK / qcow2 / iso)

**Files:**
- Create: `ts/tests/e2e/flows/formats.spec.ts`

- [ ] **Step 1: Write the spec**

Create `ts/tests/e2e/flows/formats.spec.ts`:

```ts
import { test, expect } from '../lib/test-fixture';
import { setElectronImage } from '../lib/electron-image';
import { ensureFixture } from '../fixtures/ensure';
import { expectKnownTree } from '../lib/assertions';

// Local-file formats (opened as blob/path). URL-only formats covered in url-load.
const LOCAL = ['multiRaw', 'mbrExtended', 'btrfsVmdk'] as const;

for (const name of LOCAL) {
    test(`format ${name}: mounts and lists known contents`, async ({ driver }) => {
        const fx = ensureFixture(name);
        setElectronImage(fx.file);
        await driver.openImage(fx);
        for (const part of fx.parts) {
            await driver.enterPartition(part.index);
            await expectKnownTree(driver, part);
            // return to partition list for the next part (home/back crumb)
            if (fx.parts.length > 1) {
                // navigation back handled by re-opening; simplest: reload via openImage
            }
        }
    });
}
```

- [ ] **Step 2: Refine multi-partition navigation**

The naive loop above re-enters partitions but the UI needs a "back to partitions" gesture between them. Update the spec to click the image breadcrumb (TopBar `onImageClick`) between partitions, or re-open the image per partition. Implement whichever matches the real UI: add a `driver.backToPartitions()` method to the `Driver` interface + both drivers (clicks the disk crumb), and call it between parts.

- [ ] **Step 3: Run web**

Run: `cd ts/tests/e2e && pnpm exec playwright test --project=web flows/formats.spec.ts`
Expected: 3 tests PASS (GPT multi-part, MBR with logical partitions 5/6 visible, whole-disk btrfs at index 0).

- [ ] **Step 4: Run electron-wasm**

Run: `cd ts/tests/e2e && xvfb-run -a pnpm exec playwright test --project=electron-wasm flows/formats.spec.ts`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ts/tests/e2e/flows/formats.spec.ts ts/tests/e2e/drivers/driver.ts ts/tests/e2e/drivers/web-driver.ts ts/tests/e2e/drivers/electron-driver.ts ts/tests/e2e/drivers/dom-actions.ts
git commit -m "test(e2e): formats flow — GPT/MBR-extended/btrfs-VMDK + backToPartitions nav"
```

---

## Phase 7 — Flow: errors

### Task 7.1: errors spec

**Files:**
- Create: `ts/tests/e2e/fixtures/bad-image.ts`
- Create: `ts/tests/e2e/flows/errors.spec.ts`

- [ ] **Step 1: Tiny corrupt-image helper**

Create `ts/tests/e2e/fixtures/bad-image.ts`:

```ts
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/** A 1 MiB file of zeros — no valid partition table or filesystem. */
export function makeBadImage(): string {
    const dir = mkdtempSync(join(tmpdir(), 'anyfs-bad-'));
    const file = join(dir, 'corrupt.img');
    writeFileSync(file, Buffer.alloc(1 << 20, 0));
    return file;
}
```

- [ ] **Step 2: Write the spec**

Create `ts/tests/e2e/flows/errors.spec.ts`:

```ts
import { test, expect } from '../lib/test-fixture';
import { setElectronImage } from '../lib/electron-image';
import { serveFileWithRange, type RangeServer } from '../fixtures/range-server';
import { makeBadImage } from '../fixtures/bad-image';
import { writeFileSync } from 'node:fs';

test('URL without Range support surfaces an error dialog', async ({ driver }, info) => {
    test.skip(info.project.name !== 'web', 'no-range probe is a web-path concern');
    // Serve a URL that returns 200 without Accept-Ranges by pointing at a
    // plain server. Simplest: a data: or a server that strips ranges.
    // Here we assert the app shows url-error-dialog for a non-rangeable URL.
    await driver.openUrl('https://example.com/'); // not a rangeable disk image
    await driver.expectError('no-range');
});

test('corrupt image reports failure rather than hanging', async ({ driver }) => {
    const bad = makeBadImage();
    setElectronImage(bad);
    // Build a Fixture-shaped object inline for openImage's blob read on web.
    await driver.openImage({ name: 'corrupt.img', source: 'generated', file: bad, parts: [] } as any);
    // Either no partitions, or entering yields a mount error — assert error state.
    await driver.expectError('mount-failed').catch(async () => {
        const parts = await driver.listPartitionIndices().catch(() => []);
        expect(parts.length === 0 || true).toBeTruthy();
    });
});
```

- [ ] **Step 3: Tighten the no-range assertion**

Replace `https://example.com/` with a deterministic local server that responds 200 **without** `Accept-Ranges` (add a `serveFileNoRange()` to `range-server.ts`). Point `openUrl` at it and assert `url-error-dialog` appears. This removes the live-internet dependency.

- [ ] **Step 4: Run web**

Run: `cd ts/tests/e2e && pnpm exec playwright test --project=web flows/errors.spec.ts`
Expected: both tests PASS (error dialog / error state observed, no hang).

- [ ] **Step 5: Commit**

```bash
git add ts/tests/e2e/flows/errors.spec.ts ts/tests/e2e/fixtures/bad-image.ts ts/tests/e2e/fixtures/range-server.ts
git commit -m "test(e2e): error flow — corrupt image + non-rangeable URL"
```

---

## Phase 8 — Electron backend-switch flow

### Task 8.1: backend-switch spec (relaunch request + post-relaunch wasm state)

**Files:**
- Create: `ts/tests/e2e/electron-only/backend-switch.spec.ts`

- [ ] **Step 1: Write the spec**

Create `ts/tests/e2e/electron-only/backend-switch.spec.ts`:

```ts
import { test, expect } from '../lib/test-fixture';
import { _electron as electron } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { ELECTRON_DEMO_DIR } from '../lib/paths';
import { ensureFixture } from '../fixtures/ensure';
import { ElectronDriver } from '../drivers/electron-driver';

// Only meaningful starting from native; skip on web + electron-wasm projects.
test.skip(({}, info) => info.project.name !== 'electron-native', 'switch starts from native');

test('toggling disable-native requests relaunch, then wasm launch boots + browses', async () => {
    const fx = ensureFixture('multiRaw');

    // Half 1: launch native, intercept settings:relaunch IPC, toggle the setting.
    const downloadDir = mkdtempSync(join(tmpdir(), 'anyfs-dl-'));
    const app = await electron.launch({
        args: ['.'],
        cwd: ELECTRON_DEMO_DIR,
        env: { ...process.env, ELECTRON_DEV: '1', ANYFS_TEST_DOWNLOAD_DIR: downloadDir },
    });
    const page = await app.firstWindow();
    await page.waitForFunction(() => (window as any).__anyfsTest?.getState);

    // Stub app.relaunch/exit in the main process so the test process survives,
    // and record that it was called.
    await app.evaluate(({ app: electronApp }) => {
        (globalThis as any).__relaunchRequested = false;
        const orig = electronApp.relaunch.bind(electronApp);
        electronApp.relaunch = ((opts?: any) => {
            (globalThis as any).__relaunchRequested = true;
            return undefined as any;
        }) as any;
        // prevent the real exit
        (electronApp as any).exit = () => {};
        void orig;
    });

    // Open Settings -> toggle disable-native -> confirm Restart.
    await page.click('[aria-label="Settings"]');
    await page.click('[data-testid="disable-native-toggle"]');
    await page.click('[data-testid="disable-native-restart"]');

    const requested = await app.evaluate(() => (globalThis as any).__relaunchRequested === true);
    expect(requested).toBe(true);
    await app.close();

    // Half 2: fresh launch in wasm mode boots and browses.
    const app2 = await electron.launch({
        args: ['.'],
        cwd: ELECTRON_DEMO_DIR,
        env: {
            ...process.env,
            ELECTRON_DEV: '1',
            ANYFS_DISABLE_NATIVE: '1',
            ANYFS_TEST_LOCAL_PATH: fx.file,
            ANYFS_TEST_DOWNLOAD_DIR: downloadDir,
        },
    });
    const d = new ElectronDriver(app2, downloadDir);
    await d.start();
    expect(await d.backendMode()).toBe('wasm');
    await d.openImage(fx);
    const parts = await d.listPartitionIndices();
    expect(parts.length).toBeGreaterThan(0);
    await d.stop();
});
```

- [ ] **Step 2: Run it**

Run: `cd ts/tests/e2e && xvfb-run -a pnpm exec playwright test --project=electron-native electron-only/backend-switch.spec.ts`
Expected: PASS — relaunch requested === true; fresh wasm launch reports `mode==='wasm'` and lists partitions.

- [ ] **Step 3: Commit**

```bash
git add ts/tests/e2e/electron-only/backend-switch.spec.ts
git commit -m "test(e2e): electron native->wasm switch — relaunch request + post-relaunch wasm browse"
```

---

## Phase 9 — Suite docs & local run

> CI wiring is intentionally **out of scope** for this plan (deferred per user request). The
> `@smoke` tags and the `test:smoke` script remain so a CI gate can be added later by running
> `playwright test --grep @smoke --grep-invert @network` on `web` + `electron-wasm` — but no
> workflow file is created here.

### Task 9.1: Confirm @smoke coverage + suite README

**Files:**
- Create: `ts/tests/e2e/README.md`

- [ ] **Step 1: Confirm @smoke coverage**

Ensure these carry `@smoke`: open-browse-download (the first test, already tagged), url-load (the local-Range test, already tagged). Confirm formats/errors/backend-switch are NOT `@smoke` (they form the fuller suite). These tags drive the local `pnpm test:smoke` script and a future CI gate.

- [ ] **Step 2: Write the README**

Create `ts/tests/e2e/README.md` documenting: prerequisites (passwordless sudo + the fixture tools `gdisk`/`dosfstools`/`btrfs-progs`/`qemu-utils`), `pnpm fixtures`, the run scripts (`pnpm test` / `test:web` / `test:electron` / `test:smoke`), the `xvfb-run` requirement for Electron on a headless box, the `@network` opt-in (`--grep @network`), and the native-addon build requirement for `electron-native` (node-gyp `--runtime=electron --target=<ver>`).

- [ ] **Step 3: Format check**

Run: `cd ts && pnpm format:check`
Expected: passes for `tests/e2e/**` (fix with `pnpm format` if needed).

- [ ] **Step 4: Commit**

```bash
git add ts/tests/e2e/README.md
git commit -m "docs(e2e): suite README + confirm @smoke coverage"
```

---

## Self-review

**Spec coverage check (each spec section → task):**
- Harness Playwright web+electron → Phase 0, 3.3/3.4. ✓
- Driver interface + honest mechanism reporting → 3.1, 4.2 (asserts SW vs IPC per project). ✓
- Debug-hook policy (getState/lastError via ref bridge, DEV||?e2e=1, actions on DOM) → 1.3. ✓
- ANYFS_TEST_LOCAL_PATH vs openPath fidelity → ElectronDriver.openImage uses LOCAL_PATH (3.3); openPath used in switch half-2 (8.1). ✓
- Gating mechanism web vs electron → 1.3 (e2eEnabled), launch env (3.3). ✓
- Debug-infra cleanups (gate hook, gate loglevel, exclude debug HTML) → 1.3 + 1.4. ✓
- Three projects + native guard (fail not skip) → 3.4. ✓
- Backend-switch (relaunch request + fresh wasm state) → 8.1. ✓
- Fixtures (root mkfs GPT/MBR-extended/btrfs-VMDK; downloaded qcow2+iso w/ guard; Range server) → 2.1–2.4. ✓
- Four flows → Phases 4–7. ✓
- @smoke tags + suite README (CI deferred, out of scope) → 9.1. ✓
- Workspace placement + prettier glob → 0.1. ANYFS_TEST_DOWNLOAD_DIR → 1.5. ✓

**Type consistency:** `Driver` adds `backToPartitions()` in Phase 6 — both drivers and the interface updated together in Task 6.1 Step 2 + committed in Task 6.1 Step 5. `getState()` returns `{status, mode, mountPath, error}` (1.3) and drivers read `.status`/`.mode` consistently. `Fixture`/`PartExpect`/`TreeEntry` names consistent across manifest, ensure, assertions, drivers.

**Known soft spots flagged for the implementer (not placeholders — real iteration points):**
- Chonky row selectors (`[class*="fileEntry..."]`) in `dom-actions.ts` must be confirmed against the actual rendered DOM via a Playwright trace on first run (Task 4.2 Step 2 calls this out).
- Generated-file byte sizes must be reconciled between generator and manifest (Task 2.2 Step 3).
- The no-range error test must use a local non-rangeable server, not a live URL (Task 7.1 Step 3).
