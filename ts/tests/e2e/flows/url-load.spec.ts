import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { serveFileWithRange, type RangeServer } from '../fixtures/range-server';
import { expectKnownTree } from '../lib/assertions';

// The url-load journey: open an image BY URL (not a local file), driving the
// app's URLFS (HTTP Range) path. The source is the downloaded trusty qcow2
// fixture, served over a local Range-capable HTTP server — so opening it also
// exercises the QEMU qcow2 decoder, then the ext4 partition mount.
const fx = ensureFixture('qcow2Url');
const part = fx.parts[0];

let server: RangeServer;
test.beforeAll(async () => {
    server = await serveFileWithRange(fx.file);
});
test.afterAll(async () => {
    await server?.close();
});

// FINDING F9: on the electron-NATIVE backend the `driver` fixture's teardown
// (`app.close()`) HANGS ~2 min after a native QEMU+LKL mount, blowing the
// per-test timeout. A native URL load mounts the same way, so it hits the same
// shutdown defect. Gate native off here (the fixme aborts before the `driver`
// fixture is requested, so no Electron app launches and the hang can't fire).
// electron-wasm and web are unaffected. See ts/tests/e2e/FINDINGS.md F9.
test.beforeEach(({}, testInfo) => {
    test.fixme(
        testInfo.project.name === 'electron-native',
        'F9: ElectronApplication.close() hangs ~2min after a native mount',
    );
});

// FINDING F10 (FIXED 2026-05-31): a qcow2's partition table is now enumerated
// on the wasm path. The bug was a build-script gap — build_anyfs_wasm.sh
// compiled anyfs_backend.c (the QEMU-vs-raw auto-detect in anyfs_disk_add)
// WITHOUT -DANYFS_HAS_QEMU, so qcow2 opened as raw bytes (wrong size, no PT).
// Adding the macro to the shared Phase-1 CFLAGS lets auto-detect pick the QEMU
// backend; the qcow2 now decodes to its virtual size and listParts reports the
// real partition. The unconditional F10 fixme is therefore removed. (The F9
// native-teardown gate above still applies and keeps electron-native off.)
// See ts/tests/e2e/FINDINGS.md F10.

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

// The "Open URL…" DIALOG UI: the legacy CDP suite's typeUrl() fallback proved a
// user can open an image by clicking the picker's "Open URL…" button, typing
// into the aria-label="Disk image URL" input, and clicking the dialog's Open
// button — a real user journey the driver.openUrl() bridge hook bypasses.
// Ported here so the dialog UI keeps a regression guard. Scoped to web: the
// dialog is renderer DOM from the same vite-demo bundle on every shell, and
// only the web project exposes the Playwright `page` for direct DOM driving
// (electron projects get their window inside ElectronDriver).
test('open via the "Open URL…" dialog UI, browse known entry', async ({ driver, page }, testInfo) => {
    test.skip(
        testInfo.project.name !== 'web',
        'dialog DOM is identical across shells (same vite-demo bundle); proven once on web',
    );
    void driver; // fixture already navigated the page to /?e2e=1 and awaited the bridge
    await page.getByTestId('open-url-button').click();
    const input = page.getByLabel('Disk image URL');
    await input.waitFor({ state: 'visible', timeout: 30_000 });
    await input.fill(server.url);
    // The Open button probes the URL (HEAD via probeUrlAhead) before submitting;
    // the Range server answers HEAD with Accept-Ranges + CORS, so it enables.
    await page.getByTestId('url-dialog-submit').click();
    await driver.enterPartition(part.index);
    await expectKnownTree(driver, part);
});
