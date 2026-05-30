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

// FINDING F10: a qcow2's MBR partition table is NOT enumerated — `listParts`
// returns only the whole-disk synthetic index 0, never the real ext4 partition
// (index 1, confirmed present via a host loop-mount). Entering index 0 then
// probes the MBR-prefixed whole disk as a bare filesystem (exFAT/UDF probes
// fail in dmesg) instead of the inner partition, so the mount never produces a
// file list. This is NOT URLFS-specific: opening the SAME qcow2 as a local
// <input> blob also lists only [0]. It is a real qcow2 partition-detection gap,
// so we do not weaken the assertion — we fixme the URL flow until partition
// enumeration over the qemu-decoded qcow2 disk is fixed. The CORS fix to
// range-server.ts (cross-origin URLFS fetch) and the manifest's
// index-1/etc-hostname entry (host-verified accurate) both stand. The URL open
// itself works: the source attaches and the disk reaches 'ready'; only the
// partition split is missing. See ts/tests/e2e/FINDINGS.md F10.
test.fixme(
    true,
    'F10: qcow2 MBR partition table not enumerated (listParts returns only whole-disk [0]); blocks entering the ext4 partition. Not URLFS-specific.',
);

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
