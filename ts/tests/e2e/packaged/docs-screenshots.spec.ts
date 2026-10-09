import { test, expect } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import * as dom from '../drivers/dom-actions';
import { launchPackaged } from './launch';

// Regenerates the desktop screenshots in docs/screenshots/ from a packaged
// app. Skipped unless ANYFS_DOCS_IMAGE is set:
//   ANYFS_PACKAGED_EXE    packaged executable (see gui.spec.ts)
//   ANYFS_DOCS_IMAGE      a cloud image, e.g. Ubuntu's *-server-cloudimg-amd64.img
//   ANYFS_DOCS_PART       label of the partition to browse (cloudimg-rootfs)
//   ANYFS_SCREENSHOT_DIR  output directory
// Linux: run under `xvfb-run -a -s "-screen 0 1600x1000x24"`, with an emoji
// font available to fontconfig (the status bar uses emoji).
const exe = process.env.ANYFS_PACKAGED_EXE;
const image = process.env.ANYFS_DOCS_IMAGE;
const part = process.env.ANYFS_DOCS_PART ?? 'cloudimg-rootfs';
const outDir = process.env.ANYFS_SCREENSHOT_DIR ?? '.';

test('docs screenshots', async () => {
    test.skip(!exe || !image, 'set ANYFS_PACKAGED_EXE and ANYFS_DOCS_IMAGE');
    const profile = mkdtempSync(join(tmpdir(), 'anyfs-docs-'));
    const env: Record<string, string> = {
        ...(process.env as Record<string, string>),
        ANYFS_E2E: '1',
        XDG_CONFIG_HOME: profile,
        APPDATA: profile,
    };
    delete env.ELECTRON_RUN_AS_NODE;
    delete env.ELECTRON_DEV;
    delete env.ANYFS_DISABLE_NATIVE;
    const app = await launchPackaged(exe!, env);
    try {
        const page = await app.firstWindow();
        await app.evaluate(({ BrowserWindow }) => {
            BrowserWindow.getAllWindows()[0]?.setContentSize(1200, 760);
        });
        await dom.waitForBridge(page);
        expect(await page.evaluate(() => !!(window as any).anyfsNative)).toBe(true);

        await page.evaluate((p) => (window as any).__anyfsTest.openPath(p), image);
        await dom.waitForReadyOrError(page, 120_000);
        expect(await dom.backendMode(page)).toBe('native');
        const indices = await dom.listPartitionIndices(page);
        await page.mouse.move(0, 0);
        await page.screenshot({ path: join(outDir, 'desktop-partitions.png') });

        const labels = await Promise.all(indices.map((i) => dom.partitionLabel(page, i)));
        const target = indices[labels.findIndex((l) => l.includes(part))];
        expect(target, `partition ${part} in ${labels.join(', ')}`).toBeDefined();
        await dom.enterPartition(page, target);
        await page.locator(dom.ROW).first().waitFor({ state: 'visible' });
        // Rows render before their stat results; "—" marks a pending cell.
        await page.waitForFunction(
            (sel) =>
                [...document.querySelectorAll(sel)].every((r) => !r.textContent?.includes('—')),
            dom.ROW,
        );
        await page.screenshot({ path: join(outDir, 'desktop-browse.png') });

        await dom.navigateInto(page, 'etc');
        await page.getByPlaceholder('Search').fill('host');
        await dom.propertiesOf(page, 'hosts');
        // Let the context menu finish fading out behind the dialog.
        await page
            .locator('li[role="menuitem"]', { hasText: 'Properties' })
            .first()
            .waitFor({ state: 'hidden' });
        await page.mouse.move(0, 0);
        await page.screenshot({ path: join(outDir, 'desktop-properties.png') });
    } finally {
        await app.close();
    }
});
