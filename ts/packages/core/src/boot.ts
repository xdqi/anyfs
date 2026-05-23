import { AnyfsDisk } from './disk.js';
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
                'anyfs_ts_init',
                'number',
                ['number', 'number'],
                [args.memMb, args.loglevel],
            ) as number;
            if (rc !== 0) throw new Error(`anyfs_ts_init failed: ${rc}`);
            g_kernelInitialised = true;
        }
        return M;
    })();
    return g_modulePromise;
}

export async function openDisk(M: AnyfsModule, fsPath: string): Promise<AnyfsDisk> {
    const h = M.ccall('anyfs_ts_disk_open', 'number', ['string', 'number'], [fsPath, 0]) as number;
    if (h < 0) throw new Error(`disk_open(${fsPath}) failed: ${h}`);
    return new AnyfsDisk(M, h);
}

export async function haltKernel(): Promise<void> {
    if (!g_modulePromise) return;
    const M = await g_modulePromise;
    M.ccall('anyfs_ts_kernel_halt', 'number', [], []);
    g_modulePromise = null;
    g_kernelInitialised = false;
}
