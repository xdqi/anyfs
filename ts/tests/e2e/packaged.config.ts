import { defineConfig } from '@playwright/test';

// Runs packaged/*.spec.ts against an already packaged app (ANYFS_PACKAGED_EXE);
// no web server and no fixture manifest, unlike playwright.config.ts.
export default defineConfig({
    testDir: 'packaged',
    fullyParallel: false,
    workers: 1,
    timeout: 240_000,
    // One retry, for electron.launch timeouts seen on windows-2025 (see
    // packaged/launch.ts); a test that passes only on retry is reported flaky.
    retries: 1,
    expect: { timeout: 30_000 },
    reporter: [['list']],
});
