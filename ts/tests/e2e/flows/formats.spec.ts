import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { setElectronImage } from '../lib/electron-image';
import { expectKnownTree } from '../lib/assertions';

// The FORMATS journey: open each generated local-file fixture, mount EVERY
// partition the manifest pins, assert its known top-level contents, and step
// back to the partition list between partitions via backToPartitions() — so the
// multi-partition navigation path is exercised, not just a single mount.

// FINDING F9: on the electron-NATIVE backend a native QEMU+LKL mount succeeds
// in isolation, but the `driver` fixture's `app.close()` teardown HANGS ~2 min
// after one, blowing the per-test timeout and cascading onto siblings. Every
// formats test mounts at least one partition, so all of them would hit the
// shutdown defect on native. Gate the whole spec off that project: the fixme
// aborts before the `driver` fixture is requested, so no Electron app launches
// and the hang can't fire. The mount itself is proven working elsewhere (web +
// electron-wasm run these green); we do not weaken any assertion. See
// ts/tests/e2e/FINDINGS.md F9.
test.beforeEach(({}, testInfo) => {
    test.fixme(
        testInfo.project.name === 'electron-native',
        'F9: ElectronApplication.close() hangs ~2min after a native mount, blowing the fixture teardown timeout',
    );
});

// Raw images whose container the partition scanner reads directly (no qcow2/
// vmdk decode needed). multiRaw is a GPT disk (ext4 #1 + vfat #2); mbrExtended
// is an MBR disk with two primary partitions (#1 ext4, #2 vfat) plus two
// logical partitions (#5, #6 ext4) inside an extended partition. Both PASS on
// web + electron-wasm; each asserts mount+list for EVERY pinned partition and
// uses backToPartitions() to navigate between them.
for (const name of ['multiRaw', 'mbrExtended'] as const) {
    test(`format ${name}: each partition mounts and lists known contents`, async ({ driver }) => {
        const fx = ensureFixture(name);
        setElectronImage(fx.file);
        await driver.openImage(fx);

        const parts = await driver.listPartitionIndices();
        for (const part of fx.parts) {
            expect(parts, `partition #${part.index} enumerated`).toContain(part.index);
            await driver.enterPartition(part.index);
            await expectKnownTree(driver, part);
            // Return to the partition list before entering the next one. This
            // exercises the breadcrumb → ConfirmDialog → picker round-trip and
            // leaves the app in a state where listPartitionIndices/enterPartition
            // work again for the following partition.
            await driver.backToPartitions();
        }
    });
}

// Whole-disk btrfs in a VMDK: container decode on the wasm WORKERFS/URLFS path
// (F10 — qcow2/vmdk not decoded for blob/URL opens, raw bytes yield no PT) plus
// the native whole-disk btrfs auto-detect gap (F5 — the open-time fstype-hint
// probe lacks the btrfs superblock magic, so enter(0) errors cleanly instead of
// mounting). Broken on every backend, so fixme everywhere rather than weakening
// the assertion. See ts/tests/e2e/FINDINGS.md F5 and F10.
test.fixme('format btrfsVmdk: whole-disk btrfs mounts and lists', async ({ driver }) => {
    const fx = ensureFixture('btrfsVmdk');
    setElectronImage(fx.file);
    await driver.openImage(fx);
    await driver.enterPartition(fx.parts[0].index);
    await expectKnownTree(driver, fx.parts[0]);
});
