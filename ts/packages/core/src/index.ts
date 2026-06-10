/**
 * @anyfs/core — wasm-LKL anyfs binding for browsers.
 *
 * `mountFile(file, opts)` spawns a dedicated Web Worker that hosts the wasm
 * runtime. Two reasons the wasm MUST live in a worker:
 *   1. WORKERFS.mount() asserts ENVIRONMENT_IS_WORKER.
 *   2. LKL's blocking syscalls call Atomics.wait, which Chrome forbids on
 *      the main JS thread.
 *
 * For Node tests, import `@anyfs/core/node` instead — that entry uses NODEFS
 * directly without a worker (Node has no Atomics.wait restriction).
 */
import type { SessionOpts } from './types.js';
import { WasmSession } from './wasm-session.js';
import { NativeSession, getAnyfsNative } from './native-session.js';
import { getUrlProxyPrefix } from './electron-proxy.js';

// ── Public API surface ────────────────────────────────

export { WasmSession } from './wasm-session.js';
export { NativeSession, getAnyfsNative } from './native-session.js';
export type { AnyfsNativeBridge } from './native-session.js';
export { NodeWasmSession } from './node-wasm-session.js';
export type { AnyfsSession } from './session.js';
export { AnyfsSessionBase } from './session-base.js';
export { applyUrlProxy, getUrlProxyPrefix } from './electron-proxy.js';
export { createSession } from './dispatch.js';
export type { WasmCaps, SessionEnv, SessionBackend, DispatchResult } from './dispatch.js';
export { fmtBytes, fmtMode, fmtTime, fmtDev, formatSize, splitExt } from './format.js';

// Re-export types
export type {
    SessionHandle,
    LklFd,
    SessionPartInfo,
    DirEntry,
    EntryKind,
    Stat,
    SessionOpts,
    SessionMeta,
    SessionSource,
} from './types.js';

export interface BrowserMountOpts extends SessionOpts {
    /** URL to the worker module (the bundle output `wasm/anyfs.worker.js`).
     *  Required because the worker can't statically import siblings under
     *  arbitrary bundlers; consumers serve it from their app root. */
    workerUrl: string | URL;
    /** URL prefix where the wasm shim and `.wasm` file live; the worker
     *  dynamic-imports `${wasmBaseUrl}${wasmModuleName}`. Defaults to
     *  `/wasm/`. */
    wasmBaseUrl?: string;
    /** Filename of the emscripten shim under wasmBaseUrl. Defaults to
     *  `anyfs.mjs`. The bundle always includes the QEMU block layer
     *  (qcow2/vmdk/vdi/vhd in addition to raw). */
    wasmModuleName?: string;
}

/** Spawn the worker and boot the LKL kernel without attaching a disk yet.
 *  Use this to pay the wasm download + kernel boot cost during your landing
 *  page; call `session.attachBlob(blob)` once the user selects a file. */
export async function prewarm(opts: BrowserMountOpts): Promise<WasmSession> {
    // eslint-disable-next-line no-console
    console.log('[PREWARM] creating worker, url=', String(opts.workerUrl));
    const worker = new Worker(opts.workerUrl, { type: 'module' });
    try {
        // eslint-disable-next-line no-console
        console.log('[PREWARM] waiting for host-ready...');
        await WasmSession.waitForReady(worker);
        // eslint-disable-next-line no-console
        console.log('[PREWARM] host-ready received');
    } catch (err) {
        worker.terminate();
        throw err;
    }
    const session = new WasmSession(worker);
    try {
        // eslint-disable-next-line no-console
        console.log('[PREWARM] calling boot...');
        await session.callRaw('boot', {
            memMb: opts.memMb ?? 64,
            loglevel: opts.loglevel ?? 0,
            wasmBaseUrl: opts.wasmBaseUrl ?? '/wasm/',
            wasmModuleName: opts.wasmModuleName ?? 'anyfs.mjs',
            // Forward whatever the host (preload / contextBridge) advertised
            // to the renderer — the worker has no preload of its own.
            urlProxyPrefix: getUrlProxyPrefix(),
        });
        // eslint-disable-next-line no-console
        console.log('[PREWARM] boot complete');
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}

/** Browser entry — mount a Blob via a worker-hosted WORKERFS.
 *  Equivalent to `prewarm(opts)` followed by `session.attachBlob(blob)`. */
export async function mountBlob(blob: Blob, opts: BrowserMountOpts): Promise<WasmSession> {
    const session = await prewarm(opts);
    try {
        await session.attachBlob(blob);
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}

/** Native entry — boot the host-side LKL kernel via the preload-injected
 *  `window.anyfsNative` IPC bridge. Returns `null` when the bridge isn't
 *  installed (plain browser / non-Electron Node) so callers can fall back
 *  to `prewarm()` transparently.
 *
 *  Unlike the wasm path this does NOT auto-attach a disk — File / URL
 *  sources have no in-kernel WORKERFS / URLFS analogue on the native side.
 *  Use `session.attachPath(hostFsPath)` for a real host filesystem path
 *  (typically obtained from the renderer's dialog/main-process picker).
 *
 *  The host kernel is process-global and idempotently booted; calling
 *  `prewarmNative()` from multiple renderers / components is safe. */
export async function prewarmNative(
    opts: Pick<SessionOpts, 'memMb' | 'loglevel'> = {},
): Promise<NativeSession | null> {
    const bridge = getAnyfsNative();
    if (!bridge) return null;
    const ok = await bridge.available();
    if (!ok) return null;
    const session = new NativeSession(bridge);
    try {
        await session.boot(opts.memMb ?? 256, opts.loglevel ?? 0);
        return session;
    } catch (err) {
        await session.close();
        throw err;
    }
}
