import type { Download, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import type { Fixture } from '../fixtures/manifest';
import type { Driver, DownloadResult, ErrorKind, PropsInfo, RowInfo } from './driver';
import * as dom from './dom-actions';

export class WebDriver implements Driver {
    constructor(private page: Page) {}

    async start(): Promise<void> {
        // ?e2e=1 activates the __anyfsTest bridge on the production preview.
        await this.page.goto('/?e2e=1');
        await dom.waitForBridge(this.page);
    }

    async stop(): Promise<void> {
        // Nothing to tear down; the Playwright fixture closes the page/context.
    }

    async openImage(fx: Fixture): Promise<void> {
        // Feed a real File from disk through the hidden <input type=file> the
        // FilePicker always keeps mounted (testid legacy-file-input). This
        // avoids serialising multi-MB image bytes through page.evaluate — the
        // generated multi.img is 64 MiB. setInputFiles works on hidden inputs.
        // The input's onChange routes into acceptBlob → fileToSource → onSource,
        // exactly the production load path.
        const input = this.page.locator('[data-testid="legacy-file-input"]');
        await input.waitFor({ state: 'attached', timeout: 30_000 });
        await input.setInputFiles(fx.file);

        // Confirm the source actually took: status must leave 'idle'. If the
        // legacy input is ignored (e.g. an FSA-only build path), fall back to
        // the __anyfsTest.setSourceFile evaluate path with a real File built
        // from the bytes we read here.
        try {
            await dom.waitForSourceLoaded(this.page, 15_000);
        } catch {
            const buf = readFileSync(fx.file);
            await this.page.evaluate(
                async ({ bytes, name }) => {
                    const file = new File([new Uint8Array(bytes)], name);
                    (window as any).__anyfsTest.setSourceFile(file);
                },
                { bytes: Array.from(buf), name: fx.name },
            );
            await dom.waitForSourceLoaded(this.page, 120_000);
        }
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
        // → the service-worker path on web (Content-Disposition: attachment via
        // a hidden iframe → SW intercept). Capture the browser download event,
        // then read the bytes off the saved file.
        const row = dom.rowByName(this.page, name);
        await row.waitFor({ state: 'visible', timeout: 30_000 });
        const [download] = await Promise.all([
            this.page.waitForEvent('download', { timeout: 120_000 }),
            row.dblclick(),
        ]);
        const path = await (download as Download).path();
        if (!path) throw new Error('download produced no file path');
        const bytes = new Uint8Array(readFileSync(path));
        return { bytes, size: bytes.byteLength, mechanism: 'service-worker' };
    }

    async expectError(kind: ErrorKind): Promise<void> {
        return dom.expectError(this.page, kind);
    }

    async backendMode(): Promise<string | null> {
        return dom.backendMode(this.page);
    }
}
