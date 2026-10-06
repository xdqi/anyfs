/** Node-only entry — uses NODEFS. Browser code should NOT import this. */
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import type { SessionOpts } from './types.js';
import { bootModule, openNodeSession, haltKernel as halt } from './boot.js';

export interface NodeMountOpts extends SessionOpts {
    /** Open the image read-only (ANYFS_SESSION_READONLY), as the browser
     *  worker does. Default false. */
    readOnly?: boolean;
}

let g_hostDir: string | null = null;

/** Boot the process-global wasm kernel with host directory `hostDir`
 *  mounted at /work (NODEFS). Later calls reuse the first kernel: their
 *  memMb / loglevel are ignored, and a different hostDir throws. A failed
 *  boot stays cached, so later calls reject the same way until
 *  `haltKernel()` resets it. */
export async function bootNodeKernel(
    hostDir: string,
    factory: AnyfsModuleFactory,
    opts: Pick<SessionOpts, 'memMb' | 'loglevel'> = {},
): Promise<AnyfsModule> {
    const { default: path } = await import('node:path');
    const dir = path.resolve(hostDir);
    if (g_hostDir !== null && g_hostDir !== dir) {
        throw new Error(
            `bootNodeKernel: /work is already ${g_hostDir}; one process mounts one host directory`,
        );
    }
    g_hostDir = dir;
    return bootModule({
        factory,
        memMb: opts.memMb ?? 64,
        loglevel: opts.loglevel ?? 0,
        preRun: [
            (m: AnyfsModule) => {
                if (!m.NODEFS) throw new Error('NODEFS not exported');
                m.FS.mkdir('/work');
                m.FS.mount(m.NODEFS, { root: dir }, '/work');
            },
        ],
    });
}

export async function mountNodeFile(
    hostPath: string,
    factory: AnyfsModuleFactory,
    opts: NodeMountOpts = {},
) {
    const { default: path } = await import('node:path');
    // NODEFS exposes a symlink as-is and emscripten resolves its target
    // inside the wasm FS, where it doesn't exist: mount the real directory.
    const { realpath } = await import('node:fs/promises');
    const real = await realpath(hostPath);
    const M = await bootNodeKernel(path.dirname(real), factory, opts);
    return openNodeSession(M, `/work/${path.basename(real)}`, {
        opTimeoutMs: opts.opTimeoutMs,
        readOnly: opts.readOnly,
    });
}

export { openNodeSession };
export async function haltKernel(): Promise<void> {
    await halt();
    g_hostDir = null;
}
