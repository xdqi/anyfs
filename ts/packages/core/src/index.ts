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
import type { MountOpts } from './types.js';
import { WorkerAnyfsDisk } from './worker-client.js';

export type { WorkerAnyfsDisk as AnyfsDisk, DiskSource } from './worker-client.js';
export type {
    DiskHandle,
    LklFd,
    PartInfo,
    DirEntry,
    EntryKind,
    Stat,
    MountOpts,
    EnterOpts,
    DiskMeta,
} from './types.js';

export interface BrowserMountOpts extends MountOpts {
    /** URL to the worker module (the bundle output `wasm/anyfs.worker.js`).
     *  Required because the worker can't statically import siblings under
     *  arbitrary bundlers; consumers serve it from their app root. */
    workerUrl: string | URL;
    /** URL prefix where the wasm shim and `.wasm` file live; the worker
     *  dynamic-imports `${wasmBaseUrl}${wasmModuleName}`. Defaults to
     *  `/wasm/`. */
    wasmBaseUrl?: string;
    /** Filename of the emscripten shim under wasmBaseUrl. Defaults to
     *  `anyfs.mjs`. Set to `anyfs.qemu.mjs` to use the QEMU-libblock bundle
     *  (qcow2/vmdk/vdi/vhd in addition to raw images). */
    wasmModuleName?: string;
}

/** Spawn the worker and boot the LKL kernel without attaching a disk yet.
 *  Use this to pay the wasm download + kernel boot cost during your landing
 *  page; call `disk.attach(file)` once the user selects a file. */
export async function prewarm(opts: BrowserMountOpts): Promise<WorkerAnyfsDisk> {
    const worker = new Worker(opts.workerUrl, { type: 'module' });
    try {
        await WorkerAnyfsDisk.waitForReady(worker);
    } catch (err) {
        worker.terminate();
        throw err;
    }
    const client = new WorkerAnyfsDisk(worker);
    try {
        await client.callRaw('boot', {
            memMb: opts.memMb ?? 64,
            loglevel: opts.loglevel ?? 0,
            wasmBaseUrl: opts.wasmBaseUrl ?? '/wasm/',
            wasmModuleName: opts.wasmModuleName ?? 'anyfs.mjs',
        });
        return client;
    } catch (err) {
        await client.dispose();
        throw err;
    }
}

/** Browser entry — mount a File/Blob via a worker-hosted WORKERFS.
 *  Equivalent to `prewarm(opts)` followed by `disk.attach(file)`. */
export async function mountFile(file: File, opts: BrowserMountOpts): Promise<WorkerAnyfsDisk> {
    const client = await prewarm(opts);
    try {
        await client.attach(file);
        return client;
    } catch (err) {
        await client.dispose();
        throw err;
    }
}
