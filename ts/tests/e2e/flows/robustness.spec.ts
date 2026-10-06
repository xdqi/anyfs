import { test, expect } from '../lib/test-fixture';
import type { Driver } from '../drivers/driver';
import { ensureFixture } from '../fixtures/ensure';
import { robustnessCase } from '../fixtures/robustness';

// Corrupt images from the robustness corpus (ts/tests/robustness). What is
// tested is the wasm sandbox promise: a hostile image ends in a visible
// error, never a hang, and the app then opens the next image normally.
// Web and electron-wasm only: native crashes are findings
// (ts/tests/robustness/FINDINGS.md), so playwright.config.ts keeps this file
// off the electron-native project.
//
// No fatal case: after the errors= hardening no corpus image ends in a fatal.
// The fatal path (abort or watchdog -> onFatal -> provider error) is covered
// by the Node harness and the @anyfs/core and @anyfs/react unit tests.

/** Truncated to 25 %: ext4 rejects the geometry at mount (session_enter rc=-22). */
const ERROR_CASE = 'ext4-trunc-25';
/** Mounts and lists its root, but the docs/ inode is zeroed (lstat rc=-74). */
const READ_CASE = 'ext4-zero-docs-inode';

const good = ensureFixture('singleExt4');

/** After any failure: close, open a good image, see its partitions. */
async function expectRecovery(driver: Driver): Promise<void> {
    await driver.close();
    await driver.openImage(good);
    await expect.poll(() => driver.listPartitionIndices(), { timeout: 90_000 }).toEqual([0]);
}

test('corrupt image that cannot mount: clean error, session stays healthy', async ({ driver }) => {
    await driver.openImage(robustnessCase(ERROR_CASE));
    expect(await driver.listPartitionIndices()).toEqual([0]);
    // enterPartition waits for a file list that never comes; don't await it.
    void driver.enterPartition(0).catch(() => {});
    await driver.expectError('mount-failed');
    expect(await driver.status()).toBe('ready');
    await expectRecovery(driver);
});

test('image that mounts but fails on read: error shown, session stays healthy', async ({
    driver,
}) => {
    await driver.openImage(robustnessCase(READ_CASE));
    expect(await driver.listPartitionIndices()).toEqual([0]);
    await driver.enterPartition(0);
    expect((await driver.listRows()).map((r) => r.name)).toContain('docs');
    await driver.navigateInto('docs');
    await driver.expectError('read-failed');
    expect(await driver.status()).toBe('ready');
    await expectRecovery(driver);
});
