import { test, expect } from '../lib/test-fixture';
import type { Driver } from '../drivers/driver';
import { ensureFixture } from '../fixtures/ensure';
import { serveFileWithRange, type RangeServer } from '../fixtures/range-server';

// Switching the loaded image, native module disabled (wasm). These guard the
// provider session-lifecycle + DiskView state-reset fixes (FINDINGS F16):
//   - F16-01/02: an abandoned/superseded attach must not strand a disposed
//     worker in `prewarmed`, which permanently wedged the NEXT open with
//     "AnyfsSession: already disposed" (or a stuck "attaching").
//   - F16-10 / BUG-2: a switch must drop the previous disk's partition list and
//     selected partition, not carry them onto the new disk.
//
// Driven over two local Range servers (multi.img: parts [0,1,2]; single-ext4:
// whole-disk only [0]) through the test bridge — the switch logic lives in the
// backend-agnostic React layer, so this reproduces the user's remote-URL report
// deterministically without the LAN dependency.
const multi = ensureFixture('multiRaw');
const single = ensureFixture('singleExt4');

let multiSrv: RangeServer;
let singleSrv: RangeServer;
test.beforeAll(async () => {
    multiSrv = await serveFileWithRange(multi.file);
    singleSrv = await serveFileWithRange(single.file);
});
test.afterAll(async () => {
    await multiSrv?.close();
    await singleSrv?.close();
});

// FINDING F9: electron-native teardown (app.close()) hangs ~2min after a native
// QEMU+LKL mount. Gate it off here (the fixme aborts before the driver fixture
// launches the app). web + electron-wasm exercise the same renderer logic.
test.beforeEach(({}, testInfo) => {
    test.fixme(
        testInfo.project.name === 'electron-native',
        'F9: ElectronApplication.close() hangs ~2min after a native mount',
    );
});

// Poll the partition picker until it reflects the EXPECTED disk. listParts is
// the "reached ready on the right disk" signal: DiskView is keyed by source, so
// the new disk's partition buttons replace the old set once it mounts. Polling
// (vs a one-shot status read) sidesteps the ready→attaching→ready transition
// race when one disk supersedes another.
function expectPartitions(driver: Driver, want: number[]) {
    return expect.poll(() => driver.listPartitionIndices(), { timeout: 90_000 }).toEqual(want);
}

test('@smoke switching disks shows the NEW disk, not the previous partitions', async ({
    driver,
}) => {
    await driver.openUrl(multiSrv.url);
    await expectPartitions(driver, [0, 1, 2]);

    await driver.openUrl(singleSrv.url);
    // The picker must reflect single-ext4 (whole-disk only), not multi's [0,1,2].
    await expectPartitions(driver, [0]);
});

test('reopen after an abandoned mid-attach switch recovers (no "already disposed" wedge)', async ({
    driver,
}) => {
    // Fire A then immediately B: B supersedes A while A is still booting/
    // attaching. Pre-fix this stranded A's disposed worker in `prewarmed`.
    await driver.openUrl(multiSrv.url); // A (not awaited — attach in flight)
    await driver.openUrl(singleSrv.url); // B supersedes A
    await expectPartitions(driver, [0]);

    // The clean third open is where the wedge fired pre-fix ("AnyfsSession:
    // already disposed"). It must now reach ready and list multi's partitions.
    await driver.openUrl(multiSrv.url);
    await expectPartitions(driver, [0, 1, 2]);
});

test('open → close → reopen recovers cleanly', async ({ driver }) => {
    await driver.openUrl(multiSrv.url);
    await expectPartitions(driver, [0, 1, 2]);

    await driver.close();
    await driver.openUrl(singleSrv.url);
    await expectPartitions(driver, [0]);
});

test('switching while inside a partition returns to the new disk’s picker (BUG-2)', async ({
    driver,
}) => {
    await driver.openUrl(multiSrv.url);
    await expectPartitions(driver, [0, 1, 2]);
    await driver.enterPartition(1); // go INSIDE partition #1 (selectedPart = 1)

    // Switch to a disk that has no partition #1. Pre-fix, the stale selectedPart
    // carried over and DiskView tried to mount a nonexistent partition #1 of the
    // new disk (empty/"Nothing to show"), never returning to the picker. Post-fix
    // selectedPart resets and the single-ext4 whole-disk picker [0] renders.
    await driver.openUrl(singleSrv.url);
    await expectPartitions(driver, [0]);
});
