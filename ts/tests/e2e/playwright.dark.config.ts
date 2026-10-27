import { defineConfig } from '@playwright/test';
import { VITE_DEMO_DIR } from './lib/paths';

const PORT = 4199;

export default defineConfig({
    testDir: '.',
    fullyParallel: false,
    workers: 1,
    timeout: 60_000,
    expect: { timeout: 15_000 },
    reporter: [['list']],
    projects: [
        {
            name: 'web-dark',
            testMatch: ['flows/theme.spec.ts'],
            use: { baseURL: 'http://localhost:' + PORT },
        },
    ],
    webServer: {
        command: 'pnpm --filter vite-demo dev --host 127.0.0.1 --port ' + PORT,
        cwd: VITE_DEMO_DIR,
        port: PORT,
        reuseExistingServer: false,
        timeout: 120_000,
    },
});
