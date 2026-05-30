import type { ElectronApplication, Page } from '@playwright/test';
import { readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import type { Fixture } from '../fixtures/manifest';
import type { Driver, DownloadResult, ErrorKind, PropsInfo, RowInfo } from './driver';
import * as dom from './dom-actions';

/**
 * Drives the REAL Electron app via Playwright's `_electron`. The renderer DOM
 * is identical to the web build (same vite-demo bundle), so partition / file /
 * properties / error actions all delegate to the shared dom-actions module.
 * What differs from WebDriver is the source-injection and download mechanics:
 *
 *  - Source injection: in NATIVE mode we set a {kind:'path'} source via the
 *    `__anyfsTest.openPath` bridge (the same source the "Open file…" button's
 *    `dialog:openImage` IPC would produce, with path === ANYFS_TEST_LOCAL_PATH),
 *    which drives the real native attachPath. We use the bridge rather than the
 *    button because the button's recents write hangs on a broken IndexedDB
 *    (FINDING F6). In WASM mode (native bridge disabled) there is no path load,
 *    so we feed the file through the hidden legacy <input>, mirroring WebDriver.
 *  - Downloads go through the `download:open/write/close` IPC, which writes to
 *    ANYFS_TEST_DOWNLOAD_DIR (set by launchElectron). We trigger the download
 *    via the DOM, wait for the in-app DownloadStatus UI to complete, then read
 *    the file back off the temp dir. mechanism is always 'electron-ipc'.
 */
export class ElectronDriver implements Driver {
    private page!: Page;

    constructor(
        private app: ElectronApplication,
        private downloadDir: string,
    ) {}

    async start(): Promise<void> {
        this.page = await this.app.firstWindow();
        // The renderer is loaded with ?e2e=1 (main.ts appends it when ANYFS_E2E
        // is set), so e2eEnabled() is true and the __anyfsTest bridge installs.
        // Wait for getState to be callable before any driver action.
        await dom.waitForBridge(this.page);
    }

    async stop(): Promise<void> {
        await this.app.close();
    }

    async openImage(fx: Fixture): Promise<void> {
        // Decide the load path by which backend the renderer resolved. In
        // native mode we load the host path; in wasm mode there is no path
        // attach, so we feed the image bytes through the test bridge exactly
        // like the web fallback does.
        const native = await this.isNativeMode();
        if (native) {
            // We use the __anyfsTest.openPath bridge rather than clicking the
            // "Open file…" button. The button path is functionally identical
            // — both set a {kind:'path'} source that drives the SAME native
            // attachPath/mount pipeline — but the button's onOpenFile first
            // awaits addRecentPath(), which hangs forever in the packaged
            // build because IndexedDB never settles on the anyfs:// origin
            // (FINDING F6). openPath sets the exact same source the IPC would
            // produce (the path === ANYFS_TEST_LOCAL_PATH), so the native
            // backend drive is real; only the broken recents bookkeeping is
            // skipped.
            await this.page.evaluate((p) => (window as any).__anyfsTest.openPath(p), fx.file);
        } else {
            // Wasm fallback: no host-path load. Feed a real File through the
            // hidden <input type=file> the FilePicker always keeps mounted
            // (testid legacy-file-input) — exactly like WebDriver. setInputFiles
            // streams the file from disk, avoiding serialising the (multi-MiB)
            // image bytes through page.evaluate. onChange → acceptBlob →
            // fileToSource → onSource is the production browser load path.
            const input = this.page.locator('[data-testid="legacy-file-input"]');
            await input.waitFor({ state: 'attached', timeout: 30_000 });
            await input.setInputFiles(fx.file);
        }
        // Prewarm leaves status at 'booted' before any source attaches, so a
        // bare 'status !== idle' check would pass prematurely. Wait for the
        // attach to actually finish ('ready') or fail ('error').
        await dom.waitForReadyOrError(this.page, 120_000);
    }

    async openUrl(url: string): Promise<void> {
        await this.page.evaluate((u) => (window as any).__anyfsTest.openUrl(u), url);
    }

    async listPartitionIndices(): Promise<number[]> {
        return dom.listPartitionIndices(this.page);
    }

    async enterPartition(index: number): Promise<void> {
        return dom.enterPartition(this.page, index);
    }

    async backToPartitions(): Promise<void> {
        return dom.backToPartitions(this.page);
    }

    async listRows(): Promise<RowInfo[]> {
        return dom.listRows(this.page);
    }

    async navigateInto(name: string): Promise<void> {
        return dom.navigateInto(this.page, name);
    }

    async propertiesOf(name: string): Promise<PropsInfo> {
        return dom.propertiesOf(this.page, name);
    }

    async download(name: string): Promise<DownloadResult> {
        // Activating a file (double-click) fires onFileActivate → streamDownload
        // → the Electron bridge path (window.electronDownload), which opens the
        // download via the download:open IPC. With ANYFS_TEST_DOWNLOAD_DIR set,
        // main.ts writes to <downloadDir>/<name> and skips the Save dialog.
        const row = dom.rowByName(this.page, name);
        await row.waitFor({ state: 'visible', timeout: 30_000 });
        await row.dblclick();

        // The DownloadingFileTree shows DownloadStatus (data-testid
        // "download-status") while the IPC stream is in flight and removes it
        // (setActive(null)) once handle.promise resolves — i.e. download:close
        // has flushed every byte to disk. Wait for it to appear then detach.
        const status = this.page.locator('[data-testid="download-status"]');
        try {
            await status.waitFor({ state: 'visible', timeout: 30_000 });
        } catch {
            // A tiny file can stream + close before we observe the status node.
            // That's fine — fall through to the on-disk size-stabilise wait.
        }
        // If an error surfaced, fail loudly rather than reading a partial file.
        const errBox = this.page.locator('[data-testid="download-error"]');
        if (await errBox.count()) {
            const msg = (await errBox.first().textContent())?.trim() ?? 'unknown';
            throw new Error(`electron download reported error: ${msg}`);
        }
        await status.waitFor({ state: 'detached', timeout: 120_000 });

        // The file is now fully written to the temp download dir.
        const filePath = join(this.downloadDir, name);
        await this.waitForStableFile(filePath);
        const bytes = new Uint8Array(readFileSync(filePath));
        return { bytes, size: bytes.byteLength, mechanism: 'electron-ipc' };
    }

    async expectError(kind: ErrorKind): Promise<void> {
        return dom.expectError(this.page, kind);
    }

    async backendMode(): Promise<string | null> {
        return dom.backendMode(this.page);
    }

    // ── helpers ────────────────────────────────────────────────────────────

    /**
     * Whether the renderer resolved the native backend. At picker time (before a
     * source is loaded) getState().mode is still null, so we read the same signal
     * the renderer itself uses: getAnyfsNative() returns the bridge iff
     * window.anyfsNative is present (preload only exposes it when
     * ANYFS_DISABLE_NATIVE is unset). nativeMode also requires
     * !settings.disableNative, which defaults false in the test profile.
     */
    private async isNativeMode(): Promise<boolean> {
        return this.page.evaluate(() => !!(window as any).anyfsNative);
    }

    /** Poll until the file exists and its size stops growing (download flushed). */
    private async waitForStableFile(path: string, timeout = 120_000): Promise<void> {
        const deadline = Date.now() + timeout;
        let lastSize = -1;
        let stableTicks = 0;
        for (;;) {
            let size = -1;
            try {
                size = statSync(path).size;
            } catch {
                size = -1;
            }
            if (size >= 0 && size === lastSize) {
                if (++stableTicks >= 3) return;
            } else {
                stableTicks = 0;
            }
            lastSize = size;
            if (Date.now() > deadline) {
                if (size < 0) throw new Error(`download file never appeared: ${path}`);
                return; // size known but never declared stable — return what we have
            }
            await new Promise((r) => setTimeout(r, 50));
        }
    }
}
