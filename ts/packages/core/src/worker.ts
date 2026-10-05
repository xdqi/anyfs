/**
 * @anyfs/core worker entry — hosts the entire wasm module inside a Web Worker
 * so that:
 *   - WORKERFS.mount() passes its assert(ENVIRONMENT_IS_WORKER) check
 *   - LKL's blocking syscalls (sem_wait → Atomics.wait) work — Chrome only
 *     permits Atomics.wait inside workers, not on the main JS thread.
 *
 * This worker owns the module, so it must never block: every op runs on
 * the bundle's API thread (see wasm-api.ts) while this thread stays free to
 * serve the file-system calls other pthreads proxy to it — the QEMU
 * thread's reads of the WORKERFS / URLFS image among them.
 *
 * Protocol: postMessage({id, op, args}) → postMessage({id, ok, result|error}).
 */
/// <reference lib="webworker" />

import { createUrlFs } from './url-fs.js';
import { setUrlProxyPrefix } from './electron-proxy.js';
import { ApiOp, wasmApiFor, type WasmApi, type WasmApiModule } from './wasm-api.js';

console.log('[WORKER_V3] anyfs.worker.js loaded at ' + Date.now());

declare const self: DedicatedWorkerGlobalScope;

interface AnyMod extends WasmApiModule {
    FS: {
        mkdir(p: string): void;
        mount(t: unknown, o: unknown, m: string): void;
        unmount(m: string): void;
        rmdir(p: string): void;
    };
    WORKERFS?: unknown;
}

let M: AnyMod | null = null;
let api: WasmApi | null = null;
let diskHandle = -1;
let readBuf = 0; // pre-allocated scratch buffer for pread, lifetime = mount session
let readBufSize = 0;

const send = (m: unknown) => self.postMessage(m);

/** Tear down the /work mount + dir so a fresh attach starts clean. Best-effort:
 *  unmount throws if nothing is mounted, rmdir throws if absent/non-empty — both
 *  are fine to ignore. Without this, a previous (even failed) attach leaves
 *  /work mounted, so the next attach's `FS.mkdir('/work')` throws EEXIST and the
 *  worker is permanently un-attachable. */
function resetWorkFs(): void {
    if (!M) return;
    try {
        M.FS.unmount('/work');
    } catch {
        /* not mounted */
    }
    try {
        M.FS.rmdir('/work');
    } catch {
        /* absent or busy */
    }
}

self.addEventListener('error', (e: ErrorEvent) => {
    send({ event: 'host-error', message: e.message, stack: (e.error as Error | undefined)?.stack });
});
self.addEventListener('unhandledrejection', (e: PromiseRejectionEvent) => {
    const r = e.reason as { message?: string; stack?: string } | undefined;
    send({ event: 'host-rejection', message: r?.message ?? String(e.reason), stack: r?.stack });
});

function needApi(): WasmApi {
    if (!api) throw new Error('not mounted');
    return api;
}

/** Open the image at fsPath read-only (neither WORKERFS nor URLFS can take
 *  writebacks); a failure carries the backend's message. */
async function openSession(fsPath: string): Promise<number> {
    const a = needApi();
    const h = await a.call(ApiOp.SESSION_OPEN, [fsPath, 1]);
    if (h < 0) {
        const why = await a.lastError();
        throw new Error(`anyfs_ts_session_open failed: ${why || `rc=${h}`}`);
    }
    return h;
}

type BootArgs = {
    memMb?: number;
    loglevel?: number;
    wasmBaseUrl?: string;
    wasmModuleName?: string;
    urlProxyPrefix?: string;
};
type AttachArgs = { blob: Blob };
type AttachUrlArgs = { url: string; name: string };
type MountArgs = BootArgs & AttachArgs;

const ops: Record<string, (a: any) => unknown> = {
    async boot(a: BootArgs) {
        if (M) return { alreadyBooted: true };
        // Install the host URL-proxy hint onto the worker's own globalThis
        // before URLFS runs — that's the same lookup applyUrlProxy() uses
        // in the renderer (which gets it from preload's contextBridge).
        setUrlProxyPrefix(a.urlProxyPrefix);
        const memMb = a.memMb ?? 64;
        const loglevel = a.loglevel ?? 0;
        const wasmBaseUrl = a.wasmBaseUrl ?? '/wasm/';
        const wasmModuleName = a.wasmModuleName ?? 'anyfs.mjs';
        send({ event: 'progress', step: 'importing wasm shim' });
        const mod = await import(/* @vite-ignore */ `${wasmBaseUrl}${wasmModuleName}`);
        const factory = mod.default as (opts: unknown) => Promise<AnyMod>;

        send({ event: 'progress', step: 'instantiating wasm' });
        M = await factory({
            print: (m: string) => send({ event: 'stdout', message: m }),
            printErr: (m: string) => send({ event: 'stderr', message: m }),
            locateFile: (p: string) => new URL(`${wasmBaseUrl}${p}`, self.location.href).href,
            onAbort: (r: unknown) => send({ event: 'abort', reason: String(r) }),
        });

        api = wasmApiFor(M);
        // Scratch buffer for pread, allocated once per boot.
        readBufSize = 1 << 20;
        readBuf = M._malloc(readBufSize);

        send({ event: 'progress', step: 'booting kernel' });
        const ic = await api.call(ApiOp.KERNEL_INIT, [memMb, loglevel]);
        if (ic !== 0) throw new Error(`anyfs_ts_kernel_init failed: ${ic}`);
        send({ event: 'progress', step: 'kernel ready' });
        return { ok: true };
    },

    async attach(a: AttachArgs) {
        if (!M) throw new Error('attach: kernel not booted (call boot first)');
        if (diskHandle >= 0) throw new Error('attach: already attached');
        // a.blob is typed as Blob but in browser workers it's always a File
        // (File extends Blob, adds .name). Cast so we can access .name.
        const file = a.blob as File;
        const fsPath = `/work/${file.name || 'image'}`;
        send({ event: 'progress', step: 'attaching disk image' });
        if (!M.WORKERFS) throw new Error('WORKERFS missing');
        // Start from a clean /work even if a prior attach left a stale mount.
        resetWorkFs();
        try {
            M.FS.mkdir('/work');
            M.FS.mount(
                M.WORKERFS,
                {
                    blobs: [{ name: file.name || 'image', data: file }],
                },
                '/work',
            );

            send({ event: 'progress', step: 'opening disk' });
            diskHandle = await openSession(fsPath);
            return { diskHandle };
        } catch (e) {
            // Leave the worker reusable: drop the half-built handle + mount so a
            // retry on this same worker doesn't hit 'already attached'/EEXIST.
            await ops.detach();
            throw e;
        }
    },

    async attachUrl(a: AttachUrlArgs) {
        send({
            event: 'stderr',
            message: `[diag] attachUrl entered, url=${a.url}, name=${a.name}, M=${!!M}`,
        });
        if (!M) throw new Error('attachUrl: kernel not booted (call boot first)');
        if (diskHandle >= 0) throw new Error('attachUrl: already attached');
        const fsPath = `/work/${a.name || 'image'}`;
        send({ event: 'stderr', message: `[diag] attachUrl fsPath=${fsPath}` });
        send({ event: 'progress', step: 'probing URL' });
        const URLFS = createUrlFs(M);
        send({ event: 'stderr', message: '[diag] attachUrl URLFS created ok' });
        // Start from a clean /work even if a prior attach left a stale mount.
        resetWorkFs();
        try {
            M.FS.mkdir('/work');
            M.FS.mount(URLFS, { url: a.url, name: a.name || 'image' }, '/work');
            send({
                event: 'stderr',
                message: '[diag] attachUrl URLFS mounted, calling disk_open...',
            });
            send({ event: 'progress', step: 'opening disk' });
            diskHandle = await openSession(fsPath);
            send({ event: 'stderr', message: `[diag] attachUrl disk_open returned ${diskHandle}` });
            send({ event: 'stderr', message: '[diag] attachUrl done, returning diskHandle' });
            return { diskHandle };
        } catch (e) {
            await ops.detach();
            throw e;
        }
    },

    // Back-compat: boot + attach in one shot.
    async mount(a: MountArgs) {
        const bootRet = (await ops.boot(a)) as { ok?: boolean; alreadyBooted?: boolean };
        if (!bootRet?.ok && !bootRet?.alreadyBooted) throw new Error('boot failed');
        return await ops.attach({ blob: a.blob });
    },

    listParts() {
        return needApi().callJsonOut(ApiOp.SESSION_LIST, [diskHandle], 'session_list_json');
    },

    meta() {
        return needApi().callJsonOut(ApiOp.SESSION_META, [diskHandle], 'session_meta_json');
    },

    async enter({ part, flags }: { part: number; flags?: number }) {
        if (!M) throw new Error('not mounted');
        const a = needApi();
        const cap = 128;
        const out = M._malloc(cap);
        try {
            const rc = await a.call(ApiOp.SESSION_ENTER, [diskHandle, part, flags ?? 1, out, cap]);
            if (rc < 0) {
                const why = await a.lastError();
                throw new Error(`disk_enter rc=${rc}${why ? `: ${why}` : ''}`);
            }
            return M.UTF8ToString(out);
        } finally {
            M._free(out);
        }
    },

    readdir({ path }: { path: string }) {
        return needApi().callJsonOut(ApiOp.READDIR, [path], 'readdir_json');
    },

    stat({ path }: { path: string }) {
        return needApi().callJsonOut(ApiOp.LSTAT, [path], 'lstat_json');
    },

    // Follow-symlinks stat, needed so openReadable's Content-Length matches
    // the bytes actually streamable from the file (lstat on a symlink reports
    // the link-target string length, which truncates the download stream).
    statFollow({ path }: { path: string }) {
        return needApi().callJsonOut(ApiOp.STAT, [path], 'stat_json');
    },

    // Read the verbatim target string of a symlink.
    async readlink({ path }: { path: string }) {
        if (!M) throw new Error('not mounted');
        // PATH_MAX is 4096 on Linux; longer can't be represented anyway.
        const cap = 4096;
        const buf = M._malloc(cap);
        try {
            const n = await needApi().call(ApiOp.READLINK, [path, buf, cap]);
            if (n < 0) throw new Error(`readlink rc=${n}`);
            return M.UTF8ToString(buf, n);
        } finally {
            M._free(buf);
        }
    },

    // Canonicalize a directory path: follow all symlink hops, return the
    // absolute LKL path. Only valid for directories. Returns the negative
    // errno verbatim so callers can fall back (e.g. -ENOTDIR on a file).
    async realpath({ path }: { path: string }) {
        if (!M) throw new Error('not mounted');
        // PATH_MAX is 4096 on Linux; longer can't be represented anyway.
        const cap = 4096;
        const buf = M._malloc(cap);
        try {
            const n = await needApi().call(ApiOp.REALPATH, [path, buf, cap]);
            if (n < 0) throw new Error(`realpath rc=${n}`);
            return M.UTF8ToString(buf, n);
        } finally {
            M._free(buf);
        }
    },

    // Read a small text file from the in-kernel namespace (e.g.
    // /proc/filesystems). One wasm call instead of open+pwrite×N+close.
    readKernelFile({ path }: { path: string }) {
        return needApi().callStringOut(ApiOp.READ_KERNEL_FILE, [path], 'readKernelFile', 4096);
    },

    open({ path }: { path: string }) {
        return needApi().call(ApiOp.OPEN, [path, 0]);
    },

    async read({ fd, offset, length }: { fd: number; offset: number; length: number }) {
        if (!M) throw new Error('not mounted');
        // Reads land in the boot-time scratch buffer; clamp to its size.
        const want = Math.min(length, readBufSize);
        const offBig = BigInt(offset);
        const lo = Number(offBig & 0xffffffffn) | 0;
        const hi = Number((offBig >> 32n) & 0xffffffffn) | 0;
        const n = await needApi().call(ApiOp.PREAD, [fd, readBuf, want, lo, hi]);
        if (n < 0) throw new Error(`pread rc=${n}`);
        return new Uint8Array(M.HEAPU8.subarray(readBuf, readBuf + n).slice());
    },

    close({ fd }: { fd: number }) {
        return needApi().call(ApiOp.CLOSE, [fd]);
    },

    /** Release the current disk + /work mount but keep the kernel booted, so
     *  this worker can be reused for another attach. Idempotent. */
    async detach() {
        if (!M) return 0;
        if (diskHandle >= 0) {
            try {
                await needApi().call(ApiOp.SESSION_CLOSE, [diskHandle]);
            } catch {
                /* best effort */
            }
            diskHandle = -1;
        }
        resetWorkFs();
        return 0;
    },

    async dispose() {
        if (!M) return 0;
        if (diskHandle >= 0) {
            try {
                await needApi().call(ApiOp.SESSION_CLOSE, [diskHandle]);
            } catch {
                /* best effort */
            }
            diskHandle = -1;
        }
        try {
            await needApi().call(ApiOp.KERNEL_HALT);
        } catch {
            /* best effort */
        }
        if (readBuf) {
            try {
                M._free(readBuf);
            } catch {
                /* best effort */
            }
            readBuf = 0;
            readBufSize = 0;
        }
        M = null;
        api = null;
        return 0;
    },
};

// Ops are serialized: one op in flight at a time, in message order. (The API
// thread runs queued requests one by one anyway; this also keeps an op's
// multi-call sequences — e.g. a failed open plus its lastError() — together.)
let opChain: Promise<void> = Promise.resolve();

self.addEventListener('message', (e: MessageEvent) => {
    const { id, op, args } = e.data as { id: number; op: string; args: unknown };
    send({ event: 'stderr', message: `[diag] rcvd op=${op} id=${id}` });
    opChain = opChain.then(async () => {
        const t0 = performance.now();
        try {
            const fn = ops[op];
            if (!fn) throw new Error(`unknown op: ${op}`);
            const result = await fn(args || {});
            const dt = (performance.now() - t0).toFixed(1);
            send({ event: 'stderr', message: `[worker] op=${op} id=${id} took ${dt}ms` });
            send({ id, ok: true, result });
        } catch (err) {
            const er = err as { message?: string; stack?: string };
            const dt = (performance.now() - t0).toFixed(1);
            send({
                event: 'stderr',
                message: `[worker] op=${op} id=${id} ERR after ${dt}ms: ${er.message ?? String(err)}`,
            });
            send({ id, ok: false, error: er.message ?? String(err), stack: er.stack });
        }
    });
});

send({ event: 'host-ready' });
