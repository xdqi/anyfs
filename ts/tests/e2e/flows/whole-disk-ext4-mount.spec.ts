import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { setElectronImage } from '../lib/electron-image';
import { expectKnownTree } from '../lib/assertions';

// Regression for the browser whole-disk ext4 mount bug (fixed in
// src/core/anyfs_session.c): a whole-disk (no partition table) image opened
// read-only must mount its filesystem read-only too. The whole-disk enter path
// used to pass the caller's flags straight through, so the browser — which
// opens the disk read-only because WORKERFS/URLFS have no writeback path —
// ended up mounting ext4 READ-WRITE. A read-write ext4 mount writes to the
// device (jbd2 journal recovery / superblock mount state), which the read-only
// backend rejects with -EIO, aborting the mount with the auto-detect "no
// fstype matched" sentinel that surfaced in the UI as "Can't mount partition
// #0 / disk_enter rc=-1". The fix makes the whole-disk enter inherit
// ANYFS_SESSION_READONLY from how the session was opened, exactly like the
// partition path (enter_fs_slot) already did; read-only adds `noload`, which
// skips recovery and lets the mount succeed.
//
// The previous suite had no whole-disk *ext4* fixture (only whole-disk btrfs,
// which mounts with `norecovery` and didn't exercise this path), so the
// regression shipped unguarded.
const fx = ensureFixture('singleExt4');
const whole = fx.parts[0]; // whole-disk -> synthetic index 0

// Electron launches its app at fixture use-time; pin the host image path first.
test.beforeEach(() => setElectronImage(fx.file));

// Same F9 teardown defect as open-browse-download: the electron-native
// fixture's app.close() hangs ~2 min after a native QEMU+LKL mount. Skip there;
// the flow runs fully on web and electron-wasm. See FINDINGS.md F9.
test.beforeEach(({}, testInfo) => {
    test.fixme(
        testInfo.project.name === 'electron-native',
        'F9: ElectronApplication.close() hangs ~2min after a native mount, blowing the fixture teardown timeout',
    );
});

test('@smoke open whole-disk ext4, mount read-only, see known files', async ({ driver }) => {
    await driver.openImage(fx);

    // A whole-disk image with no partition table lists exactly one synthetic
    // entry, index 0.
    const parts = await driver.listPartitionIndices();
    expect(parts).toEqual([0]);

    // The regression: this enter failed with disk_enter rc=-1 in the browser.
    // It must now succeed and the top-level files become visible.
    await driver.enterPartition(whole.index);
    await expectKnownTree(driver, whole);

    const rows = await driver.listRows();
    const hello = rows.find((r) => r.name === 'hello.txt');
    expect(hello, 'hello.txt present at the mount root').toBeTruthy();
    expect(hello?.isDir).toBe(false);
});
