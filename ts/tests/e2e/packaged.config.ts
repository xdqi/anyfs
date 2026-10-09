import { defineConfig } from '@playwright/test';

// Runs packaged/*.spec.ts against an already packaged app (ANYFS_PACKAGED_EXE);
// no web server and no fixture manifest, unlike playwright.config.ts.
export default defineConfig({
    testDir: 'packaged',
    fullyParallel: false,
    workers: 1,
    timeout: 180_000,
    expect: { timeout: 30_000 },
    reporter: [['list']],
});
