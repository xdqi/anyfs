import { test as base } from '@playwright/test';
import type { Driver } from '../drivers/driver';
import { WebDriver } from '../drivers/web-driver';
import { ElectronDriver } from '../drivers/electron-driver';
import { launchElectron } from './launch-electron';
import { assertNativeAddonPresent } from './native-guard';
import { getElectronImage } from './electron-image';

/**
 * Per-project `driver` fixture. Resolves the right Driver from the running
 * project's name:
 *  - web:             WebDriver over Playwright's `page` (from the webServer).
 *  - electron-native: ElectronDriver over a freshly launched native Electron.
 *  - electron-wasm:   ElectronDriver over a freshly launched wasm Electron.
 *
 * WebDriver.start() navigates to /?e2e=1 itself; ElectronDriver.start() grabs
 * app.firstWindow() internally — so the fixture just constructs, start()s,
 * hands over, and stop()s. For electron projects `page` is unused (Electron
 * supplies its own window). The image path for electron must be pinned via
 * setElectronImage() (in a flow's beforeEach) BEFORE the fixture runs, since
 * launchElectron needs it at launch time.
 */
export const test = base.extend<{ driver: Driver }>({
    driver: async ({ page }, use, testInfo) => {
        const project = testInfo.project.name;
        if (project === 'web') {
            const d = new WebDriver(page);
            await d.start();
            await use(d);
            await d.stop();
            return;
        }
        const backend = project === 'electron-native' ? 'native' : 'wasm';
        if (backend === 'native') assertNativeAddonPresent();
        const { app, downloadDir } = await launchElectron(backend, getElectronImage());
        const d = new ElectronDriver(app, downloadDir);
        try {
            await d.start();
            await use(d);
        } finally {
            // Always close the launched Electron app, even if start() throws
            // mid-way (otherwise the process leaks). Closing `app` directly is
            // robust regardless of how far start() got; d.stop() does the same.
            await app.close();
        }
    },
});

export { expect } from '@playwright/test';
