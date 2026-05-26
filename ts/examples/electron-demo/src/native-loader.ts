/*
 * Runtime native-addon loader for the packaged Electron main process.
 *
 * The main process is bundled by esbuild (see esbuild.main.mjs) so the
 * packaged app does not depend on node_modules — pnpm's symlink store
 * makes electron-packager unable to ship transitive deps reliably in a
 * monorepo (electron/forge#4188, electron/packager#1213).
 *
 * Native `.node` files cannot be bundled, so they are staged at fixed
 * paths by scripts/stage-native.sh and resolved here at runtime. In dev
 * we fall back to the workspace build outputs so `pnpm dev` still works.
 */
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { createRequire } from 'node:module';

const req = createRequire(__filename);

function pickFirstExisting(candidates: string[]): string | null {
    for (const c of candidates) if (existsSync(c)) return c;
    return null;
}

// Packaged: resources/app/dist/main.cjs → resources/native/<addon>.node
function packagedNativeDir(): string {
    return join(process.resourcesPath, 'native');
}

export function resolveAnyfsNativeAddon(): string | null {
    return pickFirstExisting([
        join(packagedNativeDir(), 'anyfs_native.node'),
        // dev: examples/electron-demo/dist/ → packages/anyfs-native/build/Release/
        resolve(
            __dirname,
            '..',
            '..',
            '..',
            'packages',
            'anyfs-native',
            'build',
            'Release',
            'anyfs_native.node',
        ),
    ]);
}

export function resolveDrivelistNode(): string | null {
    return pickFirstExisting([
        join(packagedNativeDir(), 'drivelist.node'),
        // dev: examples/electron-demo/dist/ → node_modules/drivelist/build/Release/
        resolve(__dirname, '..', 'node_modules', 'drivelist', 'build', 'Release', 'drivelist.node'),
    ]);
}

export function loadAnyfsNativeAddon(): unknown | null {
    const p = resolveAnyfsNativeAddon();
    if (!p) return null;
    return req(p);
}

// Load `drivelist`'s JS entry. esbuild bundles drivelist's JS into main.cjs
// (with `bindings` aliased to ./bindings-shim) so we just import it like a
// regular module; the dynamic require below stays out of esbuild's analysis.
export function loadDrivelistModule(): typeof import('drivelist') {
    // The literal 'drivelist' import was rewritten by main.ts to a typed
    // import only; the actual runtime module comes from this require call,
    // which esbuild WILL bundle because it's a static string.
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    return require('drivelist');
}
