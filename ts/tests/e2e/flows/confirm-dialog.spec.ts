import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { setElectronImage } from '../lib/electron-image';

// F16-09: ConfirmDialog listened for Enter on `window`, so Enter confirmed no
// matter which button had focus. Tabbing to Cancel and pressing Enter ran
// onConfirm (from the window listener) before the button's own click ran
// onCancel: the user asked to cancel and the action happened anyway. The
// dialog is renderer DOM, identical in Electron, so this runs on web only.
const fx = ensureFixture('multiRaw');
const ext4 = fx.parts.find((p) => p.fs === 'ext4')!;

test.beforeEach(() => setElectronImage(fx.file));

test('Enter acts on the focused button of a confirm dialog', async ({ driver, page }, testInfo) => {
    test.skip(
        testInfo.project.name !== 'web',
        'renderer-only behaviour, driven through the web page',
    );
    await driver.openImage(fx);
    await driver.enterPartition(ext4.index);

    const back = page.locator(
        'nav[aria-label="Breadcrumb"] button[title="Return to the partition list"]',
    );
    const dialog = page.locator('[role="alertdialog"]');
    const partitions = page.locator('[data-testid^="partition-"]');

    // Enter on Cancel cancels: the dialog closes and the partition stays open.
    await back.click();
    await dialog.getByRole('button', { name: 'Cancel', exact: true }).focus();
    await page.keyboard.press('Enter');
    await expect(dialog).toBeHidden();
    await expect(back).toBeVisible();
    await expect(partitions).toHaveCount(0);

    // Enter on the default (focused) button still confirms.
    await back.click();
    await expect(dialog.getByRole('button', { name: 'Back', exact: true })).toBeFocused();
    await page.keyboard.press('Enter');
    await expect(dialog).toBeHidden();
    await expect(partitions.first()).toBeVisible();
});
