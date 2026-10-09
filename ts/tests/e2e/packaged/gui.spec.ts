import { test, expect, _electron } from '@playwright/test';
import { mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import * as dom from '../drivers/dom-actions';

// GUI smoke for a PACKAGED desktop app (electron-demo/scripts/package.sh
// output), on the smoke fixture from make-smoke-fixture.sh:
//   ANYFS_PACKAGED_EXE      the packaged executable (anyfs-demo, anyfs-demo.exe,
//                           or anyfs-demo.app/Contents/MacOS/anyfs-demo)
//   ANYFS_PACKAGED_FIXTURE  directory holding smoke.qcow2 + expected.json
//   ANYFS_SCREENSHOT_DIR    optional: save window screenshots there
// The renderer must come up on the native backend; a package that fell back
// to wasm fails here.
const exe = process.env.ANYFS_PACKAGED_EXE;
const fixtureDir = process.env.ANYFS_PACKAGED_FIXTURE;
const shotDir = process.env.ANYFS_SCREENSHOT_DIR;

test('packaged app opens an image natively and browses a partition', async () => {
    test.skip(!exe || !fixtureDir, 'set ANYFS_PACKAGED_EXE and ANYFS_PACKAGED_FIXTURE');
    const expected = JSON.parse(readFileSync(join(fixtureDir!, 'expected.json'), 'utf8')) as {
        image: string;
        partitions: { fstype: string; label: string }[];
        part: string;
        entries: string[];
    };
    const image = resolve(fixtureDir!, expected.image);

    // A fresh profile: a stale disableNative setting in localStorage would
    // silently switch the app to wasm.
    const profile = mkdtempSync(join(tmpdir(), 'anyfs-packaged-'));
    const env: Record<string, string> = {
        ...(process.env as Record<string, string>),
        ANYFS_E2E: '1',
        XDG_CONFIG_HOME: profile,
        APPDATA: profile,
    };
    delete env.ELECTRON_RUN_AS_NODE;
    delete env.ELECTRON_DEV;
    delete env.ANYFS_DISABLE_NATIVE;

    const app = await _electron.launch({
        executablePath: exe!,
        args: process.platform === 'linux' ? ['--no-sandbox'] : [],
        env,
    });
    try {
        const page = await app.firstWindow();
        await app.evaluate(({ BrowserWindow }) => {
            BrowserWindow.getAllWindows()[0]?.setContentSize(1280, 800);
        });
        await dom.waitForBridge(page);
        expect(await page.evaluate(() => !!(window as any).anyfsNative)).toBe(true);

        // System drives: main's drives:list (staged drivelist addon) through
        // the preload bridge into the dialog. Every runner has a system disk.
        await page.getByText('Open system drive…').click();
        const drives = page.getByRole('dialog', { name: 'System drives' });
        await expect(drives.locator('code').first()).toBeVisible();
        expect(await drives.innerText()).not.toContain('not available');
        if (shotDir) await page.screenshot({ path: join(shotDir, 'packaged-drives.png') });
        await page.keyboard.press('Escape');
        await expect(drives).toBeHidden();

        await page.evaluate((p) => (window as any).__anyfsTest.openPath(p), image);
        await dom.waitForReadyOrError(page, 120_000);
        const st = await dom.getState(page);
        expect(st?.status, st?.error?.message).toBe('ready');
        expect(await dom.backendMode(page)).toBe('native');

        const indices = await dom.listPartitionIndices(page);
        const labels = await Promise.all(indices.map((i) => dom.partitionLabel(page, i)));
        const rows = await Promise.all(
            indices.map((i) => page.locator(`[data-testid="partition-${i}"]`).innerText()),
        );
        for (const p of expected.partitions) {
            const i = labels.findIndex((l) => l.includes(p.label));
            expect(
                i,
                `partition ${p.label} in picker (${labels.join(', ')})`,
            ).toBeGreaterThanOrEqual(0);
            expect(rows[i]).toContain(p.fstype);
        }
        if (shotDir) await page.screenshot({ path: join(shotDir, 'packaged-partitions.png') });

        const target = indices[labels.findIndex((l) => l.includes(expected.part))];
        await dom.enterPartition(page, target);
        const names = (await dom.listRows(page)).map((r) => r.name);
        for (const e of expected.entries) expect(names).toContain(e);
        if (shotDir) await page.screenshot({ path: join(shotDir, 'packaged-files.png') });
    } finally {
        await app.close();
    }
});
