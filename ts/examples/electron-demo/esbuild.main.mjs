/*
 * Bundle the Electron main and preload processes for packaging.
 *
 * Why bundle: pnpm's symlink store + monorepo layout makes
 * electron-packager unable to ship transitive deps reliably
 * (electron/forge#4188, electron/packager#1213). By bundling everything
 * into a single main.cjs / preload.cjs, the packaged app has no runtime
 * dependency on node_modules at all — we only need to stage the two
 * native `.node` files at known paths (scripts/stage-native.sh).
 */
import { build } from 'esbuild';
import { rmSync } from 'node:fs';
import { resolve } from 'node:path';

rmSync('dist', { recursive: true, force: true });

const common = {
    bundle: true,
    platform: 'node',
    format: 'cjs',
    target: 'node20',
    sourcemap: true,
    legalComments: 'none',
    // Electron is provided by the runtime; the two native .node files are
    // loaded via path.join(process.resourcesPath, ...) (see native-loader.ts)
    // so esbuild shouldn't try to follow them either.
    external: ['electron'],
    // Replace the `bindings` npm package (used by drivelist) with our own
    // tiny shim that resolves to the staged .node path.
    alias: {
        bindings: resolve('src/bindings-shim.ts'),
    },
    logLevel: 'info',
};

await Promise.all([
    build({ ...common, entryPoints: ['src/main.ts'], outfile: 'dist/main.cjs' }),
    // Preload: keep CJS — Electron loads preloads via `require()` from the
    // renderer process sandbox where the electron module is always available.
    build({ ...common, entryPoints: ['src/preload.ts'], outfile: 'dist/preload.cjs' }),
    build({
        ...common,
        entryPoints: ['src/http-proxy-worker.ts'],
        outfile: 'dist/http-proxy-worker.cjs',
    }),
]);
