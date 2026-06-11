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
import { delimiter, dirname, join, resolve } from 'node:path';

function pickFirstExisting(candidates: string[]): string | null {
    for (const c of candidates) if (existsSync(c)) return c;
    return null;
}

/**
 * Windows only: make sure the directories holding the addon's transitive DLLs
 * (liblkl.dll, libanyfs-qemublk.dll, glib, …) are on PATH before we require the
 * `.node`.
 *
 * Node loads a `.node` via `LoadLibraryExW(path, NULL,
 * LOAD_WITH_ALTERED_SEARCH_PATH)`. That flag REPLACES the exe-directory entry of
 * the DLL search order with the directory of the `.node` itself — so the addon's
 * dependent DLLs are looked for next to the `.node`, NOT next to anyfs-demo.exe
 * where copy-win64-dlls.sh actually stages them. With nothing else on the path
 * they're only found when the process's current working directory happens to be
 * the exe dir; launched from anywhere else (Start menu, a shortcut, Explorer),
 * the load fails with err=126 "module not found" and the app silently falls back
 * to wasm. Verified under wine with a LoadLibraryEx probe: empty cwd → 126; with
 * the DLL dir on PATH → loads. PATH is consulted by the standard search even
 * under LOAD_WITH_ALTERED_SEARCH_PATH, so prepending it fixes the load
 * regardless of cwd.
 *
 * We add both the exe directory (where DLLs ship in packaged builds) and the
 * `.node`'s own directory (covers any layout), idempotently.
 */
function ensureDllSearchPath(nodePath: string): void {
    if (process.platform !== 'win32') return;
    const dirs = [dirname(process.execPath), dirname(nodePath)];
    const cur = process.env.PATH ?? '';
    const parts = cur.split(delimiter);
    const missing = dirs.filter((d) => d && !parts.includes(d));
    if (missing.length) process.env.PATH = [...missing, cur].join(delimiter);
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
    ensureDllSearchPath(p);
    try {
        return require(p);
    } catch (e) {
        // Surface the REAL error (Windows err=126 "module not found" usually
        // means a transitive DLL is missing, not the .node itself). The caller
        // in main.ts otherwise collapses this to a generic message.
        const err = e as NodeJS.ErrnoException;
        console.error(
            `[native-loader] require(${p}) failed:`,
            err?.code ?? '',
            err?.message ?? String(e),
        );
        throw e;
    }
}

// Load `drivelist`'s JS entry. drivelist is an optionalDependency; on bare
// CI runners (and in the wasm-only build path) it may not be installed.
// The try/catch in the same scope is esbuild's documented escape hatch:
// esbuild defers an unresolvable require() to runtime when it is
// syntactically inside a try/catch block, so build:main won't fail on
// runners that lack the optional dep. main.ts already catches the throw
// and falls back gracefully.
export function loadDrivelistModule(): typeof import('drivelist') {
    try {
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        return require('drivelist');
    } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        throw new Error(`drivelist module not available: ${msg}`);
    }
}
