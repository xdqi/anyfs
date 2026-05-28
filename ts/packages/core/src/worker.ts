/**
 * @anyfs/core worker entry — hosts the entire wasm module inside a Web Worker
 * so that:
 *   - WORKERFS.mount() passes its assert(ENVIRONMENT_IS_WORKER) check
 *   - LKL's blocking syscalls (sem_wait → Atomics.wait) work — Chrome only
 *     permits Atomics.wait inside workers, not on the main JS thread.
 *
 * The QEMU-backed bundle uses ASYNCIFY=1 + emscripten's coroutine-wasm fiber
 * backend. Any wasm export that may hit the block layer goes through an
 * asyncify unwind/rewind cycle, and `Fibers.finishContextSwitch` discards the
 * export's return value (only `Asyncify.handleSleep` preserves it). The C
 * glue exposes `_p` variants that write their result to a caller-supplied
 * int32_t* out-pointer; we allocate one scratch out-pointer per mount and
 * read HEAP32 after every async call.
 *
 * Protocol: postMessage({id, op, args}) → postMessage({id, ok, result|error}).
 */
/// <reference lib="webworker" />

import { createUrlFs } from './url-fs.js';
import { setUrlProxyPrefix } from './electron-proxy.js';

declare const self: DedicatedWorkerGlobalScope;

interface AnyMod {
    HEAPU8: Uint8Array;
    HEAP32: Int32Array;
    _malloc(n: number): number;
    _free(p: number): void;
    ccall: (
        name: string,
        ret: 'number' | 'string' | null,
        argTypes: ReadonlyArray<'number' | 'string' | 'bigint'>,
        args: ReadonlyArray<number | string | bigint>,
        opts?: { async?: boolean },
    ) => number | string | Promise<number | string>;
    UTF8ToString(ptr: number, max?: number): string;
    FS: { mkdir(p: string): void; mount(t: unknown, o: unknown, m: string): void };
    WORKERFS?: unknown;
}

let M: AnyMod | null = null;
let diskHandle = -1;
let outp = 0; // scratch int32 out-pointer, lifetime = mount session
let readBuf = 0; // pre-allocated scratch buffer for pread, lifetime = mount session
let readBufSize = 0;

const send = (m: unknown) => self.postMessage(m);

self.addEventListener('error', (e: ErrorEvent) => {
    send({ event: 'host-error', message: e.message, stack: (e.error as Error | undefined)?.stack });
});
self.addEventListener('unhandledrejection', (e: PromiseRejectionEvent) => {
    const r = e.reason as { message?: string; stack?: string } | undefined;
    send({ event: 'host-rejection', message: r?.message ?? String(e.reason), stack: r?.stack });
});

/* Async ccall wrappers — every wasm export under the ASYNCIFY bundle must
 * pass {async:true} or emscripten throws "running asynchronously". */
async function callA(
    name: string,
    ret: 'number' | 'string' | null,
    argTypes: Array<'number' | 'string' | 'bigint'>,
    args: Array<number | string | bigint>,
): Promise<number | string> {
    if (!M) throw new Error('not mounted');
    return await (M.ccall(name, ret, argTypes, args, { async: true }) as Promise<number | string>);
}

/* _p out-pointer variant: signature gains a trailing int32_t* out; JS reads
 * HEAP32[outp>>2] after the await. */
async function callP(
    name: string,
    argTypes: Array<'number' | 'string' | 'bigint'>,
    args: Array<number | string | bigint>,
): Promise<number> {
    if (!M) throw new Error('not mounted');
    M.HEAP32[outp >> 2] = -0x7fffffff;
    await M.ccall(name, null, [...argTypes, 'number'], [...args, outp], { async: true });
    return M.HEAP32[outp >> 2];
}

type BootArgs = {
    memMb?: number;
    loglevel?: number;
    wasmBaseUrl?: string;
    wasmModuleName?: string;
    urlProxyPrefix?: string;
};
type AttachArgs = { file: File };
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

        outp = M._malloc(4);
        // Pre-allocate the read scratch buffer outside any asyncify context.
        // Per-read _malloc/_free risk triggering Memory.grow inside an
        // asyncify-suspended pread (which under -pthread queues notifications
        // to peer workers; those fire as checkMailbox callbacks during the
        // yield and tip asyncify into "import changed the state" abort).
        readBufSize = 1 << 20;
        readBuf = M._malloc(readBufSize);

        send({ event: 'progress', step: 'booting kernel' });
        const ic = (await callA(
            'anyfs_ts_init',
            'number',
            ['number', 'number'],
            [memMb, loglevel],
        )) as number;
        if (ic !== 0) throw new Error(`anyfs_ts_init failed: ${ic}`);
        send({ event: 'progress', step: 'kernel ready' });
        return { ok: true };
    },

    async attach(a: AttachArgs) {
        if (!M) throw new Error('attach: kernel not booted (call boot first)');
        if (diskHandle >= 0) throw new Error('attach: already attached');
        const fsPath = `/work/${a.file.name || 'image'}`;
        send({ event: 'progress', step: 'attaching disk image' });
        if (!M.WORKERFS) throw new Error('WORKERFS missing');
        M.FS.mkdir('/work');
        M.FS.mount(
            M.WORKERFS,
            {
                blobs: [{ name: a.file.name || 'image', data: a.file }],
            },
            '/work',
        );

        send({ event: 'progress', step: 'opening disk' });
        // Always open the underlying image read-only — WORKERFS-backed disks
        // can't accept writebacks.
        diskHandle = await callP('anyfs_ts_disk_open_p', ['string', 'number'], [fsPath, 1]);
        if (diskHandle < 0) throw new Error(`anyfs_ts_disk_open failed: ${diskHandle}`);
        return { diskHandle };
    },

    async attachUrl(a: AttachUrlArgs) {
        if (!M) throw new Error('attachUrl: kernel not booted (call boot first)');
        if (diskHandle >= 0) throw new Error('attachUrl: already attached');
        const fsPath = `/work/${a.name || 'image'}`;
        send({ event: 'progress', step: 'probing URL' });
        const URLFS = createUrlFs(M);
        M.FS.mkdir('/work');
        M.FS.mount(URLFS, { url: a.url, name: a.name || 'image' }, '/work');

        send({ event: 'progress', step: 'opening disk' });
        // Read-only — the URL backend has no writeback path.
        diskHandle = await callP('anyfs_ts_disk_open_p', ['string', 'number'], [fsPath, 1]);
        if (diskHandle < 0) throw new Error(`anyfs_ts_disk_open failed: ${diskHandle}`);
        return { diskHandle };
    },

    // Back-compat: boot + attach in one shot.
    async mount(a: MountArgs) {
        const bootRet = (await ops.boot(a)) as { ok?: boolean; alreadyBooted?: boolean };
        if (!bootRet?.ok && !bootRet?.alreadyBooted) throw new Error('boot failed');
        return await ops.attach({ file: a.file });
    },

    listPartitions() {
        return callJsonOut('anyfs_ts_disk_list_json_p', ['number'], [diskHandle]);
    },

    diskMeta() {
        return callJsonOut('anyfs_ts_disk_meta_json_p', ['number'], [diskHandle]);
    },

    async mountWhole({ fstype, flags }: { fstype?: string; flags?: number }) {
        if (!M) throw new Error('not mounted');
        const cap = 128;
        const out = M._malloc(cap);
        try {
            const rc = await callP(
                'anyfs_ts_mount_whole_p',
                ['number', 'string', 'number', 'number', 'number'],
                [diskHandle, fstype ?? '', flags ?? 1, out, cap],
            );
            if (rc < 0) throw new Error(`mount_whole rc=${rc}`);
            return M.UTF8ToString(out);
        } finally {
            M._free(out);
        }
    },

    async enter({ part, flags }: { part: number; flags?: number }) {
        if (!M) throw new Error('not mounted');
        const cap = 128;
        const out = M._malloc(cap);
        try {
            const rc = await callP(
                'anyfs_ts_disk_enter_p',
                ['number', 'number', 'number', 'number', 'number'],
                [diskHandle, part, flags ?? 1, out, cap],
            );
            if (rc < 0) throw new Error(`disk_enter rc=${rc}`);
            return M.UTF8ToString(out);
        } finally {
            M._free(out);
        }
    },

    readdir({ path }: { path: string }) {
        return callJsonOutStr('anyfs_ts_readdir_json_p', path);
    },

    stat({ path }: { path: string }) {
        return callJsonOutStr('anyfs_ts_lstat_json_p', path);
    },

    // Follow-symlinks stat, needed so openReadable's Content-Length matches
    // the bytes actually streamable from the file (lstat on a symlink reports
    // the link-target string length, which truncates the download stream).
    statFollow({ path }: { path: string }) {
        return callJsonOutStr('anyfs_ts_stat_json_p', path);
    },

    // Read the verbatim target string of a symlink.
    async readlink({ path }: { path: string }) {
        if (!M) throw new Error('not mounted');
        // PATH_MAX is 4096 on Linux; link targets can't exceed that anyway.
        const cap = 4096;
        const buf = M._malloc(cap);
        try {
            const n = await callP(
                'anyfs_ts_readlink_p',
                ['string', 'number', 'number'],
                [path, buf, cap],
            );
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
        // PATH_MAX is 4096 on Linux; any longer means the kernel can't
        // represent the path anyway.
        const cap = 4096;
        const buf = M._malloc(cap);
        try {
            const n = await callP(
                'anyfs_ts_realpath_p',
                ['string', 'number', 'number'],
                [path, buf, cap],
            );
            if (n < 0) throw new Error(`realpath rc=${n}`);
            return M.UTF8ToString(buf, n);
        } finally {
            M._free(buf);
        }
    },

    // Read a small text file from the in-kernel namespace (e.g.
    // /proc/filesystems). One wasm call instead of open+pwrite×N+close.
    async readKernelFile({ path }: { path: string }) {
        if (!M) throw new Error('not mounted');
        let cap = 4096;
        for (let i = 0; i < 5; i++) {
            const buf = M._malloc(cap);
            try {
                const n = await callP(
                    'anyfs_ts_read_kernel_file_p',
                    ['string', 'number', 'number'],
                    [path, buf, cap],
                );
                if (n >= 0) return M.UTF8ToString(buf, n);
                const need = -n;
                if (need <= cap) throw new Error(`readKernelFile rc=${n}`);
                cap = Math.max(need + 256, cap * 2);
            } finally {
                M._free(buf);
            }
        }
        throw new Error('readKernelFile: buffer too large');
    },

    open({ path }: { path: string }) {
        return callP('anyfs_ts_open_p', ['string', 'number'], [path, 0]);
    },

    async read({ fd, offset, length }: { fd: number; offset: number; length: number }) {
        if (!M) throw new Error('not mounted');
        // Use the pre-allocated scratch buffer; clamp the request so we never
        // _malloc during a read (see mount() comment).
        const want = Math.min(length, readBufSize);
        const offBig = BigInt(offset);
        const lo = Number(offBig & 0xffffffffn) >>> 0;
        const hi = Number((offBig >> 32n) & 0xffffffffn) >>> 0;
        const n = await callP(
            'anyfs_ts_pread_p',
            ['number', 'number', 'number', 'number', 'number'],
            [fd, readBuf, want, lo, hi],
        );
        if (n < 0) throw new Error(`pread rc=${n}`);
        return new Uint8Array(M.HEAPU8.subarray(readBuf, readBuf + n).slice());
    },

    close({ fd }: { fd: number }) {
        return callP('anyfs_ts_close_p', ['number'], [fd]);
    },

    async dispose() {
        if (!M) return 0;
        if (diskHandle >= 0) {
            try {
                await callA('anyfs_ts_disk_close', 'number', ['number'], [diskHandle]);
            } catch {
                /* best effort */
            }
            diskHandle = -1;
        }
        try {
            await callA('anyfs_ts_kernel_halt', 'number', [], []);
        } catch {
            /* best effort */
        }
        if (outp) {
            try {
                M._free(outp);
            } catch {
                /* best effort */
            }
            outp = 0;
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
        return 0;
    },
};

async function callJsonOut(
    name: string,
    argTypes: Array<'number' | 'string'>,
    args: Array<number | string>,
): Promise<unknown> {
    if (!M) throw new Error('not mounted');
    let cap = 8192;
    for (let i = 0; i < 6; i++) {
        const buf = M._malloc(cap);
        try {
            const n = await callP(name, [...argTypes, 'number', 'number'], [...args, buf, cap]);
            if (n >= 0) return JSON.parse(M.UTF8ToString(buf, n));
            const need = -n;
            if (need <= cap) throw new Error(`${name} negative rc=${n}`);
            cap = Math.max(need + 256, cap * 2);
        } finally {
            M._free(buf);
        }
    }
    throw new Error(`${name} keeps requesting more buffer`);
}

function callJsonOutStr(name: string, path: string): Promise<unknown> {
    return callJsonOut(name, ['string'], [path]);
}

// Ops are serialized: only one wasm call active at a time. LKL holds a single
// CPU lock and ASYNCIFY suspends the running export; if two ops were
// in-flight they would interleave inside the kernel and trip
// `bad count while changing owner` panics.
let opChain: Promise<void> = Promise.resolve();

self.addEventListener('message', (e: MessageEvent) => {
    const { id, op, args } = e.data as { id: number; op: string; args: unknown };
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
