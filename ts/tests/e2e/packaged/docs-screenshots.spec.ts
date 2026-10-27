import { test, expect } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import * as dom from '../drivers/dom-actions';
import { applyPackagedTheme, launchPackaged } from './launch';

const exe = process.env.ANYFS_PACKAGED_EXE;
const image = process.env.ANYFS_DOCS_IMAGE;
const part = process.env.ANYFS_DOCS_PART ?? 'cloudimg-rootfs';
const outDir = process.env.ANYFS_SCREENSHOT_DIR ?? '.';
const shotSuffix = process.env.ANYFS_UI_THEME === 'dark' ? '-dark' : '';

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
        await applyPackagedTheme(page);
        await dom.waitForBridge(page);
        if (process.env.ANYFS_UI_THEME === 'dark') {
            await expect
                .poll(() =>
                    page.evaluate(() => document.documentElement.classList.contains('dark')),
                )
                .toBe(true);
        }
        expect(await page.evaluate(() => !!(window as any).anyfsNative)).toBe(true);

        await page.evaluate((p) => (window as any).__anyfsTest.openPath(p), image);
        await dom.waitForReadyOrError(page, 120_000);
        expect(await dom.backendMode(page)).toBe('native');
        const indices = await dom.listPartitionIndices(page);
        await page.mouse.move(0, 0);
        await page.screenshot({ path: join(outDir, 'desktop-partitions' + shotSuffix + '.png') });

        const labels = await Promise.all(indices.map((i) => dom.partitionLabel(page, i)));
        const target = indices[labels.findIndex((l) => l.includes(part))];
        expect(target, 'partition ' + part + ' in ' + labels.join(', ')).toBeDefined();
        await dom.enterPartition(page, target);
        await page.locator(dom.ROW).first().waitFor({ state: 'visible' });
        await page.waitForFunction(
            (sel) =>
                [...document.querySelectorAll(sel)].every((r) => !r.textContent?.includes('—')),
            dom.ROW,
        );
        await page.screenshot({ path: join(outDir, 'desktop-browse' + shotSuffix + '.png') });

        await dom.navigateInto(page, 'etc');
        await page.getByPlaceholder('Search').fill('host');
        await dom.propertiesOf(page, 'hosts');
        await page
            .locator('li[role="menuitem"]', { hasText: 'Properties' })
            .first()
            .waitFor({ state: 'hidden' });
        await page.mouse.move(0, 0);
        await page.screenshot({ path: join(outDir, 'desktop-properties' + shotSuffix + '.png') });
    } finally {
        await app.close();
    }
});
