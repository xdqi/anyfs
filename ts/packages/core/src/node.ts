/** Node-only entry — uses NODEFS. Browser code should NOT import this. */
import type { AnyfsModule, AnyfsModuleFactory } from './module.js';
import type { MountOpts } from './types.js';
import { bootModule, openDisk, haltKernel as halt } from './boot.js';
import type { AnyfsDisk } from './disk.js';

export async function mountNodeFile(
    hostPath: string,
    factory: AnyfsModuleFactory,
    opts: MountOpts = {},
): Promise<AnyfsDisk> {
    const memMb = opts.memMb ?? 64;
    const loglevel = opts.loglevel ?? 0;
    const { default: path } = await import('node:path');
    const dir = path.dirname(hostPath);
    const base = path.basename(hostPath);
    const M = await bootModule({
        factory,
        memMb,
        loglevel,
        preRun: [
            (m: AnyfsModule) => {
                if (!m.NODEFS) throw new Error('NODEFS not exported');
                m.FS.mkdir('/work');
                m.FS.mount(m.NODEFS, { root: dir }, '/work');
            },
        ],
    });
    return openDisk(M, `/work/${base}`);
}

export const haltKernel = halt;
