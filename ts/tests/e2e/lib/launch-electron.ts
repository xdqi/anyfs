import { _electron, type ElectronApplication } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { ELECTRON_DEMO_DIR } from './paths';

/**
 * Launch the real Electron app (`examples/electron-demo`) under Playwright's
 * `_electron` runner, wired for E2E:
 *
 *  - We deliberately do NOT set ELECTRON_DEV: that flag makes main.ts load the
 *    Vite dev server (http://localhost:5173) and SKIP registering the anyfs://
 *    protocol. With it unset, isDev=false (the app is launched unpackaged via
 *    `electron .`), so main.ts serves the production vite-demo build from
 *    `../../vite-demo/dist` over anyfs://app/ — exactly the shipping renderer.
 *  - `ANYFS_E2E=1` tells main.ts to append `?e2e=1` to the renderer URL, which
 *    is the ONLY way `e2eEnabled()` becomes true in the built renderer
 *    (import.meta.env.DEV is false there) — without it `window.__anyfsTest` is
 *    never installed and getState()/openUrl/etc. are unreachable.
 *  - `ANYFS_TEST_DOWNLOAD_DIR` makes the `download:open` IPC write to a temp dir
 *    and skip the native Save dialog (which Playwright can't drive).
 *  - `backend='wasm'` sets `ANYFS_DISABLE_NATIVE=1` to force the wasm fallback;
 *    `backend='native'` is the default (the addon loads if present).
 *  - `localImagePath` pre-sets `ANYFS_TEST_LOCAL_PATH` so the `dialog:openImage`
 *    IPC returns it and skips the native Open dialog — clicking the open button
 *    then loads that path through the real native attachPath pipeline.
 *
 * Returns the launched app plus the temp download dir so the driver can read
 * downloaded files back off disk.
 */
export async function launchElectron(
    backend: 'native' | 'wasm',
    localImagePath?: string,
): Promise<{ app: ElectronApplication; downloadDir: string }> {
    const downloadDir = mkdtempSync(join(tmpdir(), 'anyfs-e2e-dl-'));

    const env: Record<string, string> = {
        ...(process.env as Record<string, string>),
        ANYFS_E2E: '1',
        ANYFS_TEST_DOWNLOAD_DIR: downloadDir,
    };
    // Drop vars that would derail the launch shape: ELECTRON_DEV forces the
    // Vite dev-server load path (and skips the anyfs:// protocol), and
    // ELECTRON_RUN_AS_NODE turns electron into a bare Node runtime.
    delete (env as Record<string, string | undefined>).ELECTRON_DEV;
    delete (env as Record<string, string | undefined>).ELECTRON_RUN_AS_NODE;
    if (backend === 'wasm') env.ANYFS_DISABLE_NATIVE = '1';
    if (localImagePath) env.ANYFS_TEST_LOCAL_PATH = localImagePath;

    const app = await _electron.launch({
        args: ['.'],
        cwd: ELECTRON_DEMO_DIR,
        env,
    });

    return { app, downloadDir };
}
