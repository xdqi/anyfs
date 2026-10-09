import { test, expect, _electron, type Page } from '@playwright/test';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import * as dom from '../drivers/dom-actions';
import { ElectronDriver } from '../drivers/electron-driver';

// GUI test of a PACKAGED app against the read-only test device attached by
// scripts/ci/test-device.{sh,ps1} (a virtual block device backed by
// make-device-fixture.sh's parts.img). Picks the device in "Open system
// drive…" — first the whole disk, then the reference partition's own node —
// and reads the reference file through the UI's download path.
//   ANYFS_PACKAGED_EXE       packaged executable (see gui.spec.ts)
//   ANYFS_DEVICE_JSON        <state-dir>/device.json from test-device
//   ANYFS_DEVICE_FIXTURE     directory with expected.json (make-device-fixture.sh)
//   ANYFS_SCREENSHOT_DIR     optional: save screenshots there
const exe = process.env.ANYFS_PACKAGED_EXE;
const deviceJson = process.env.ANYFS_DEVICE_JSON;
const fixtureDir = process.env.ANYFS_DEVICE_FIXTURE;
const shotDir = process.env.ANYFS_SCREENSHOT_DIR;

type Expected = {
    partitions: { fstype: string; label: string }[];
    part: string;
    read: string;
    size: number;
    sha256: string;
};

const exact = (s: string) => new RegExp(`^${s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`);

async function pickDevice(page: Page, device: string): Promise<void> {
    await page.getByText('Open system drive…').click();
    const dialog = page.getByRole('dialog', { name: 'System drives' });
    const row = dialog
        .locator('button')
        .filter({ has: page.locator('code', { hasText: exact(device) }) });
    // The list loads asynchronously (drivelist in the main process).
    try {
        await expect(row).toHaveCount(1, { timeout: 30_000 });
    } catch {
        const listed = await dialog.locator('button code').allInnerTexts();
        throw new Error(
            `${device} is not listed exactly once; the dialog lists: ${listed.join(', ')}`,
        );
    }
    await row.click();
    await dom.waitForReadyOrError(page, 120_000);
    const st = await dom.getState(page);
    expect(st?.status, st?.error?.message).toBe('ready');
    expect(await dom.backendMode(page)).toBe('native');
}

async function readAndHash(driver: ElectronDriver, x: Expected): Promise<void> {
    const names = (await driver.listRows()).map((r) => r.name);
    expect(names).toContain(x.read);
    const dl = await driver.download(x.read);
    expect(dl.size).toBe(x.size);
    expect(createHash('sha256').update(dl.bytes).digest('hex')).toBe(x.sha256);
}

test('packaged app reads the test device: whole disk and partition node', async () => {
    test.skip(
        !exe || !deviceJson || !fixtureDir,
        'set ANYFS_PACKAGED_EXE, ANYFS_DEVICE_JSON, ANYFS_DEVICE_FIXTURE',
    );
    const dev = JSON.parse(readFileSync(deviceJson!, 'utf8')) as {
        device: string;
        partitions: string[];
    };
    const x = JSON.parse(readFileSync(join(fixtureDir!, 'expected.json'), 'utf8')) as Expected;

    const profile = mkdtempSync(join(tmpdir(), 'anyfs-device-'));
    const downloadDir = mkdtempSync(join(tmpdir(), 'anyfs-device-dl-'));
    const env: Record<string, string> = {
        ...(process.env as Record<string, string>),
        ANYFS_E2E: '1',
        ANYFS_TEST_DOWNLOAD_DIR: downloadDir,
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
    const driver = new ElectronDriver(app, downloadDir);
    try {
        await driver.start();
        const page = await app.firstWindow();
        await app.evaluate(({ BrowserWindow }) => {
            BrowserWindow.getAllWindows()[0]?.setContentSize(1280, 800);
        });

        // Whole disk: the partition table comes from the device.
        await pickDevice(page, dev.device);
        const indices = await dom.listPartitionIndices(page);
        const labels = await Promise.all(indices.map((i) => dom.partitionLabel(page, i)));
        const rows = await Promise.all(
            indices.map((i) => page.locator(`[data-testid="partition-${i}"]`).innerText()),
        );
        for (const p of x.partitions) {
            const i = labels.findIndex((l) => l.includes(p.label));
            expect(i, `partition ${p.label} (${labels.join(', ')})`).toBeGreaterThanOrEqual(0);
            expect(rows[i]).toContain(p.fstype);
        }
        if (shotDir) await page.screenshot({ path: join(shotDir, 'device-partitions.png') });
        const target = indices[labels.findIndex((l) => l.includes(x.part))];
        await dom.enterPartition(page, target);
        await readAndHash(driver, x);
        if (shotDir) await page.screenshot({ path: join(shotDir, 'device-files.png') });

        // The reference partition's own device node: a filesystem without a
        // partition table, opened directly.
        const partNode = dev.partitions[target - 1];
        expect(
            partNode,
            `partition node for #${target} (${dev.partitions.join(', ')})`,
        ).toBeTruthy();
        await dom.closeDisk(page);
        await expect(page.getByText('Open system drive…')).toBeVisible();
        await pickDevice(page, partNode);
        if ((await page.locator('[data-testid^="partition-"]').count()) > 0) {
            await dom.enterPartition(page, (await dom.listPartitionIndices(page))[0]);
        }
        await readAndHash(driver, x);
    } finally {
        await app.close();
    }
});
