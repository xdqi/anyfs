import { NodeWasmSession } from './node-wasm-session.js';
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';

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
        const M = await args.factory({ preRun: args.preRun });
        if (!g_kernelInitialised) {
            const rc = M.ccall(
                'anyfs_ts_kernel_init',
                'number',
                ['number', 'number'],
                [args.memMb, args.loglevel],
            ) as number;
            if (rc !== 0) throw new Error(`anyfs_ts_kernel_init failed: ${rc}`);
            g_kernelInitialised = true;
        }
        return M;
    })();
    return g_modulePromise;
}

/** Boot the kernel and open a session for the given disk image path.
 *  NODEFS mount of the host directory must already be set up (via boot's
 *  `preRun` hook). */
export async function openNodeSession(M: AnyfsModule, fsPath: string): Promise<NodeWasmSession> {
    const session = new NodeWasmSession(M);
    await session.attachPath(fsPath);
    return session;
}

export async function haltKernel(): Promise<void> {
    if (!g_modulePromise) return;
    const M = await g_modulePromise;
    M.ccall('anyfs_ts_kernel_halt', 'number', [], []);
    g_modulePromise = null;
    g_kernelInitialised = false;
}
