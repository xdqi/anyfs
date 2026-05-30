import { test, expect } from '@playwright/test';
import { ensureFixture } from '../fixtures/ensure';
import { launchElectron } from '../lib/launch-electron';
import { assertNativeAddonPresent } from '../lib/native-guard';
import { ElectronDriver } from '../drivers/electron-driver';

// The native→wasm BACKEND SWITCH flow. The Electron app's Settings dialog has a
// "Disable native module" toggle; flipping it pops a confirm dialog whose
// "Restart" button writes the setting to localStorage and calls
// window.electronSettings.relaunch() → IPC `settings:relaunch` → main process
// app.relaunch() + app.exit(). After the relaunch the app boots with the native
// module disabled, i.e. on the wasm backend.
//
// Playwright's `_electron` cannot follow a real process re-exec, so the flow is
// proven in two halves:
//   HALF 1 — launch native, monkeypatch app.relaunch/app.exit in the MAIN
//            process (so the test process survives), open Settings, toggle the
//            switch, click Restart, and assert the relaunch was REQUESTED.
//   HALF 2 — separately launch a fresh Electron with ANYFS_DISABLE_NATIVE=1 and
//            assert it boots on the wasm backend and can open + browse an image.
const fx = ensureFixture('multiRaw');

// This is a native-START scenario: the premise is "begin on native, switch to
// wasm". It lives in electron-only/ (matched by both electron projects) but only
// makes sense on electron-native — electron-wasm has no native module to disable,
// and the `web` project's testMatch is flows/** so it never picks this up. Gate
// to electron-native so the other electron project SKIPS the whole spec.
test.beforeEach(({}, testInfo) => {
    test.skip(
        testInfo.project.name !== 'electron-native',
        'native→wasm switch flow starts from the native backend (electron-native only)',
    );
});

test('toggling disable-native requests relaunch; fresh wasm launch boots + browses', async () => {
    // electron-native MUST have an ABI-matching addon present, or fail loudly
    // (a silent skip would hide a broken native path) — same guard the shared
    // `driver` fixture applies for the native project.
    assertNativeAddonPresent();

    // ── HALF 1: native app, intercept relaunch, toggle the setting ──────────
    // No image: we only touch Settings, so the native app never mounts a disk.
    // That matters for teardown — FINDING F9 (native app.close() hangs ~2 min)
    // is specific to a native QEMU+LKL MOUNT having happened; with no mount the
    // close path stays clean. (If this app.close() ever hangs without a mount,
    // that would be a NEW finding.)
    const { app } = await launchElectron('native');
    try {
        const page = await app.firstWindow();
        // The renderer installs window.__anyfsTest only with ?e2e=1 (main.ts
        // appends it when ANYFS_E2E is set). Wait for the bridge before reading.
        await page.waitForFunction(() => !!(window as any).__anyfsTest?.getState, null, {
            timeout: 60_000,
        });
        // At picker time (before any source attaches) getState().mode is still
        // null — the renderer itself decides "native" off window.anyfsNative,
        // which preload exposes only when ANYFS_DISABLE_NATIVE is unset. Assert
        // THAT signal: we genuinely launched on the native backend.
        const nativePresent = await page.evaluate(() => !!(window as any).anyfsNative);
        expect(nativePresent).toBe(true);

        // Monkeypatch app.relaunch + app.exit in the MAIN process BEFORE clicking
        // Restart: app.evaluate runs in the Electron main context with the
        // electron module passed in. Record a flag instead of actually relaunching
        // (and stub exit) so the test process survives the click.
        await app.evaluate(({ app: a }) => {
            (globalThis as Record<string, unknown>).__relaunchRequested = false;
            a.relaunch = (() => {
                (globalThis as Record<string, unknown>).__relaunchRequested = true;
            }) as typeof a.relaunch;
            (a as unknown as { exit: () => void }).exit = () => {
                /* swallow: keep the test process alive */
            };
        });

        // Open Settings (TopBar gear button, aria-label="Settings"). The
        // "Disable native module" toggle only renders when nativeAvailable is
        // true (App.tsx passes !!getAnyfsNative()) — which holds on native.
        await page.click('[aria-label="Settings"]');
        const toggle = page.locator('[data-testid="disable-native-toggle"]');
        await toggle.waitFor({ state: 'visible', timeout: 30_000 });
        // onChange → setPending(v) + setShowConfirm(true): the confirm dialog
        // with the Restart button appears.
        await toggle.click();
        const restart = page.locator('[data-testid="disable-native-restart"]');
        await restart.waitFor({ state: 'visible', timeout: 30_000 });
        // Clicking Restart writes localStorage + calls electronSettings.relaunch()
        // → IPC settings:relaunch → our patched main-process app.relaunch().
        await restart.click();

        // Assert the relaunch was REQUESTED in the main process.
        await expect
            .poll(
                async () =>
                    app.evaluate(
                        () => (globalThis as Record<string, unknown>).__relaunchRequested === true,
                    ),
                { timeout: 30_000 },
            )
            .toBe(true);
    } finally {
        // No mount happened on this native app, so close should be clean (not
        // subject to F9, which is native-mount-specific).
        await app.close();
    }

    // ── HALF 2: fresh wasm-backend launch boots + browses ───────────────────
    // ANYFS_DISABLE_NATIVE=1 (set by launchElectron('wasm')) forces the wasm
    // worker path — exactly what the app would boot into after the relaunch.
    const { app: app2, downloadDir: dd2 } = await launchElectron('wasm', fx.file);
    const driver = new ElectronDriver(app2, dd2);
    try {
        await driver.start();
        // dispatch.backend resolves 'wasm' when the native module is disabled.
        expect(await driver.backendMode()).toBe('wasm');
        // Open + browse: the wasm electron backend feeds the image through the
        // hidden legacy <input> as a blob (the F8 fix path) and mounts it.
        await driver.openImage(fx);
        const parts = await driver.listPartitionIndices();
        expect(parts.length).toBeGreaterThan(0);
    } finally {
        // wasm-backend teardown is NOT subject to F9 (no native QEMU mount).
        await driver.stop();
    }
});
