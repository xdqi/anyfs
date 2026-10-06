/** Node-only entry — uses NODEFS. Browser code should NOT import this. */
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import type { SessionOpts } from './types.js';
import { bootModule, openNodeSession, haltKernel as halt } from './boot.js';

export interface NodeMountOpts extends SessionOpts {
    /** Open the image read-only (ANYFS_SESSION_READONLY), as the browser
     *  worker does. Default false. */
    readOnly?: boolean;
}

/** Boot the process-global wasm kernel with host directory `hostDir`
 *  mounted at /work (NODEFS). Idempotent: later calls return the same
 *  module. */
export async function bootNodeKernel(
    hostDir: string,
    factory: AnyfsModuleFactory,
    opts: SessionOpts = {},
): Promise<AnyfsModule> {
    return bootModule({
        factory,
        memMb: opts.memMb ?? 64,
        loglevel: opts.loglevel ?? 0,
        preRun: [
            (m: AnyfsModule) => {
                if (!m.NODEFS) throw new Error('NODEFS not exported');
                m.FS.mkdir('/work');
                m.FS.mount(m.NODEFS, { root: hostDir }, '/work');
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
    const M = await bootNodeKernel(path.dirname(hostPath), factory, opts);
    return openNodeSession(M, `/work/${path.basename(hostPath)}`, {
        opTimeoutMs: opts.opTimeoutMs,
        readOnly: opts.readOnly,
    });
}

export { openNodeSession };
export const haltKernel = halt;
