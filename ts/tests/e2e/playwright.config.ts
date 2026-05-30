import { defineConfig } from '@playwright/test';
import { VITE_DEMO_DIR } from './lib/paths';

// 4199 (not vite's default 4173) so we never collide with a dev-launched
// preview/caddy on 4173. Combined with reuseExistingServer:false below, every
// run spawns its own fresh `vite build && preview` — never reuses a possibly
// stale server, so test signal always reflects current code.
const WEB_PORT = 4199;

export default defineConfig({
    testDir: '.',
    fullyParallel: false,
    workers: 1,
    timeout: 120_000,
    expect: { timeout: 30_000 },
    reporter: [['list'], ['html', { open: 'never' }]],
    projects: [
        {
            name: 'web',
            testMatch: ['flows/**/*.spec.ts'],
            use: { baseURL: `http://localhost:${WEB_PORT}` },
        },
    ],
    webServer: {
        command: `pnpm --filter vite-demo build && pnpm --filter vite-demo preview --port ${WEB_PORT} --strictPort`,
        cwd: VITE_DEMO_DIR,
        port: WEB_PORT,
        // Always spawn our own fresh server; never reuse a stray one. A busy
        // port fails loudly rather than silently testing stale content.
        reuseExistingServer: false,
        timeout: 300_000,
    },
});
