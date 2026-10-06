import { NodeWasmSession, type NodeWasmSessionOpts } from './node-wasm-session.js';
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import { EngineFatalError } from './session-base.js';
import { ApiOp, wasmApiFor } from './wasm-api.js';

let g_modulePromise: Promise<AnyfsModule> | null = null;
let g_kernelInitialised = false;

export async function bootModule(args: {
    factory: AnyfsModuleFactory;
    preRun: Array<(m: AnyfsModule) => void>;
    memMb: number;
    loglevel: number;
}): Promise<AnyfsModule> {
    if (g_modulePromise) return g_modulePromise;
    g_modulePromise = (async () => {
        let live: AnyfsModule | null = null;
        let early: Error | null = null; // an abort that beat the factory
        // abort() — a kernel panic, a wasm trap, OOM — calls this on the
        // module-owning thread (emscripten proxies it from pthreads). Fail
        // the API so pending and future ops reject and every session fires
        // onFatal; the module stays dead for the life of the process.
        const onAbort = (what: unknown) => {
            const err = new EngineFatalError(`wasm module aborted: ${String(what)}`);
            if (live) wasmApiFor(live).fail(err);
            else early ??= err;
        };
        const M = await args.factory({ preRun: args.preRun, onAbort });
        live = M;
        if (early) wasmApiFor(M).fail(early);
        if (!g_kernelInitialised) {
            const rc = await wasmApiFor(M).call(ApiOp.KERNEL_INIT, [args.memMb, args.loglevel]);
            if (rc !== 0) throw new Error(`anyfs_ts_kernel_init failed: ${rc}`);
            g_kernelInitialised = true;
        }
        return M;
    })();
    return g_modulePromise;
}

/** Open a session for the given disk image path on a booted module. NODEFS
 *  mount of the host directory must already be set up (via boot's `preRun`
 *  hook). */
export async function openNodeSession(
    M: AnyfsModule,
    fsPath: string,
    opts: NodeWasmSessionOpts = {},
): Promise<NodeWasmSession> {
    const session = new NodeWasmSession(M, opts);
    try {
        await session.attachPath(fsPath);
    } catch (err) {
        await session.close().catch(() => {});
        throw err;
    }
    return session;
}

export async function haltKernel(): Promise<void> {
    if (!g_modulePromise) return;
    const M = await g_modulePromise;
    // A dead module can't answer KERNEL_HALT. (A watchdog wedge without an
    // abort makes halt hang too: recovery is a new process.)
    if (!wasmApiFor(M).failed) await wasmApiFor(M).call(ApiOp.KERNEL_HALT);
    g_modulePromise = null;
    g_kernelInitialised = false;
}
