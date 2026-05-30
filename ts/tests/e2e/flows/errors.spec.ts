import { test, expect } from '../lib/test-fixture';
import { setElectronImage } from '../lib/electron-image';
import { serveFileNoRange, type RangeServer } from '../fixtures/range-server';
import { makeBadImage } from '../fixtures/bad-image';
import { ensureFixture } from '../fixtures/ensure';
import type { Fixture } from '../fixtures/manifest';

// The ERRORS / edge-cases journey: the real property under test is GRACEFUL
// FAILURE. A source the app cannot handle (a corrupt/no-filesystem image, or a
// URL whose server can't do Range reads) must surface a visible error and NOT
// hang or crash. A hang here shows up as a bounded test timeout — which would
// itself be a finding, not a silent pass.

// FINDING F9: on the electron-NATIVE backend the `driver` fixture's teardown
// (`app.close()`) hangs ~2 min after a native QEMU+LKL mount. These error cases
// do NOT mount successfully (they error out), so F9 — which triggers on a
// successful mount's teardown — likely would not fire. But to stay consistent
// and never risk the 2-min hang, gate native off: the fixme aborts before the
// `driver` fixture is requested, so no Electron app launches. The error
// behaviour is fully covered by web + electron-wasm. See FINDINGS.md F9.
test.beforeEach(({}, testInfo) => {
    test.fixme(
        testInfo.project.name === 'electron-native',
        'F9: native app teardown hang — avoid even on error paths',
    );
});

/** Build the minimal Fixture an `openImage` needs for a generated bad image. */
function badFixture(file: string): Fixture {
    return { name: 'corrupt.img', source: 'generated', file, parts: [] };
}

test('corrupt image: reports an error / no mountable FS, does not hang', async ({ driver }) => {
    const bad = makeBadImage();
    setElectronImage(bad);
    await driver.openImage(badFixture(bad));

    // A 1 MiB all-zeros file has no partition table, so the partition scanner
    // can only offer the synthetic whole-disk index 0 (if anything). Entering
    // it probes the bare bytes for a filesystem; none exists, so the mount must
    // FAIL with a clean error rather than hanging (F2 fixed the wasm no-FS
    // hang). We assert exactly that the app surfaces a failure.
    const indices = await driver.listPartitionIndices();
    expect(indices, 'a no-PT disk offers at most the whole-disk index').toEqual([0]);

    // Entering the whole disk must error (no filesystem). enterPartition resolves
    // when EITHER a file list OR an inline mount error appears, so we race it
    // against expectError: whichever the app shows first wins. A genuine error
    // surface (status==='error' / "Can't mount partition #N") makes expectError
    // resolve; if instead a (bogus) file list rendered, enterPartition would
    // resolve and we'd fail the explicit assertion below. A hang in NEITHER path
    // trips the bounded test timeout, which is itself a finding.
    let entered = false;
    await Promise.race([
        driver.enterPartition(0).then(() => {
            entered = true;
        }),
        driver.expectError('mount-failed'),
    ]);
    if (entered) {
        // The mount "succeeded" into a view — verify it's the error surface,
        // not a real file list (a no-FS disk must never list contents).
        await driver.expectError('mount-failed');
    }
});

// The no-range URL case is a WEB/wasm URLFS concern: the in-worker URLFS does a
// HEAD then a probe `Range: bytes=0-0` GET; serveFileNoRange answers 200 (not
// 206) and omits Accept-Ranges, so URLFS throws "server lacks Accept-Ranges"
// during mount → the app errors. On Electron, URL opens go through the
// main-process proxy, which may mask this — so this is scoped to web (the real
// URLFS gate) and electron-wasm (in-worker URLFS, same as web). electron-native
// is already F9-gated above.
test('URL without Range support surfaces an error (web/wasm)', async ({ driver }) => {
    const server: RangeServer = await serveFileNoRange(ensureFixture('multiRaw').file);
    try {
        await driver.openUrl(server.url);
        await driver.expectError('no-range');
    } finally {
        await server.close();
    }
});
