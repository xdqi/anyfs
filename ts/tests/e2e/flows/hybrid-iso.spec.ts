import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { setElectronImage } from '../lib/electron-image';
import { expectKnownTree } from '../lib/assertions';

// A hybrid ISO has a filesystem on the whole disk (iso9660) and one inside a
// partition (the FAT EFI image). The kernel cannot mount a disk and its
// partition at once (EBUSY), and the UI never unmounts when it goes back to
// the partition list. So entering the whole disk after the partition, or the
// reverse, failed with "sessionEnter failed: rc=-16" (found on Windows
// native; the session layer is shared, so every backend had it). Alternate
// a few times so a stale mount or a sticky failure would show.
test('hybrid ISO: switch between the whole disk and its EFI partition', async ({ driver }) => {
    const fx = ensureFixture('isoUrl');
    setElectronImage(fx.file);
    await driver.openImage(fx);

    const whole = fx.parts.find((p) => p.index === 0)!;
    const efi = fx.parts.find((p) => p.index === 2)!;
    expect(await driver.listPartitionIndices()).toEqual(expect.arrayContaining([0, 2]));

    for (const part of [efi, whole, efi, whole]) {
        await driver.enterPartition(part.index);
        await expectKnownTree(driver, part);
        await driver.backToPartitions();
    }
});
