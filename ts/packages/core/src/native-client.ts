/**
 * Native-addon backend for @anyfs/core.
 *
 * Mirrors the public surface of WorkerAnyfsDisk so callers (AnyfsProvider,
 * vite-demo's App.tsx, openReadable consumers) don't care which path runs.
 * The wasm path is great in the browser; in Electron we'd rather skip the
 * asyncify/ucontext overhead and go straight to the host's real LKL kernel
 * via the preload-injected `window.anyfsNative` IPC bridge.
 *
 * Boot model: the addon's `init()` is one process-global LKL kernel. The
 * IPC handler in electron-demo/src/main.ts makes init idempotent, so
 * concurrent NativeAnyfsDisk instances all share the same kernel — that's
 * fine because each disk gets its own handle (0..N) from `diskOpen`.
 *
 * Things that don't translate from wasm:
 *  - `attach(file: File)`: WORKERFS isn't a thing in main process. For now
 *    we throw if called with a File — vite-demo's electron path either
 *    passes URLs (anyfs:// or anyfs-url://) or uses the file-on-disk path
 *    available via the future `attachPath()` extension. The wasm worker
 *    fallback handles File transparently.
 *  - `onProgress`: addon ops aren't streamed; this is a no-op subscriber.
 *    The progress events were only really meaningful for the multi-second
 *    wasm boot anyway.
 *  - WORKERFS-specific behaviors (zero-copy slice etc): pread returns a
 *    fresh Uint8Array per call, same as the worker.
 */

import type { DirEntry, DiskHandle, DiskMeta, EnterOpts, LklFd, PartInfo, Stat } from './types.js';

/** Shape of the preload-injected bridge. Async because IPC is async. */
export interface AnyfsNativeBridge {
    available(): Promise<boolean>;
    init(memMb: number, loglevel: number): Promise<number>;
    diskOpen(path: string, flags: number): Promise<number>;
    diskClose(h: number): Promise<number>;
    diskListJson(h: number): Promise<string>;
    diskMetaJson(h: number): Promise<string>;
    diskEnter(h: number, part: number, flags: number): Promise<string>;
    mountWhole(h: number, fstype: string, flags: number): Promise<string>;
    readdirJson(path: string): Promise<string>;
    lstatJson(path: string): Promise<string>;
    statJson(path: string): Promise<string>;
    realpath(path: string): Promise<string>;
    readlink(path: string): Promise<string>;
    registerUrl(url: string): Promise<{ proxyUrl: string; id: string }>;
    unregisterUrl(id: string): Promise<void>;
    fileOpen(path: string, flags: number): Promise<number>;
    pread(fd: number, n: number, off: number): Promise<{ rc: number; data: Uint8Array }>;
    fileClose(fd: number): Promise<number>;
}

/** Returns the host-injected native bridge, or null if unavailable. */
export function getAnyfsNative(): AnyfsNativeBridge | null {
    try {
        const g = (globalThis as unknown as { anyfsNative?: AnyfsNativeBridge }).anyfsNative;
        if (g && typeof g.init === 'function') return g;
    } catch {
        /* sandboxed contextBridge sometimes throws on access */
    }
    return null;
}

export class NativeAnyfsDisk {
    private readonly bridge: AnyfsNativeBridge;
    private handle: DiskHandle = -1;
    private disposed = false;
    private proxyId: string | null = null;
    private readonly fds = new Set<LklFd>();
    // Serialize ops so a slow readdir doesn't interleave with a pread on
    // the same kernel — matches the worker's `opChain` discipline. The
    // native side could in principle parallelize, but anyfs_ts holds the
    // LKL CPU lock per call anyway, so concurrency would just queue inside.
    private opChain: Promise<unknown> = Promise.resolve();

    constructor(bridge: AnyfsNativeBridge) {
        this.bridge = bridge;
    }

    /** Boot the addon's kernel (idempotent in the main process). */
    async boot(memMb: number, loglevel: number): Promise<void> {
        const rc = await this.bridge.init(memMb, loglevel);
        if (rc !== 0) throw new Error(`anyfs-native init failed: rc=${rc}`);
    }

    private chain<T>(fn: () => Promise<T>): Promise<T> {
        if (this.disposed) return Promise.reject(new Error('AnyfsDisk: already disposed'));
        const next = this.opChain.then(fn, fn);
        // Swallow rejection so a failed op doesn't poison the chain for
        // subsequent ops (the caller still sees the rejection via `next`).
        this.opChain = next.catch(() => undefined);
        return next;
    }

    /** Open a host-side path as the disk image. The Electron renderer can
     *  reach paths via the dialog/main-process path picker.
     *
     *  Browser-style `attach(File)` is not supported here; vite-demo's
     *  electron-aware mount path must hand us a real filesystem path. */
    async attachPath(path: string): Promise<void> {
        if (this.handle >= 0) throw new Error('attachPath: already attached');
        const h = await this.chain(() => this.bridge.diskOpen(path, 1));
        if (h < 0) throw new Error(`diskOpen failed: rc=${h}`);
        this.handle = h;
    }

    /** Compatibility shim — wasm-path callers use `attach(file: File)`.
     *  The native bridge has no WORKERFS, so we reject explicitly so the
     *  caller can fall back to the worker path. */
    attach(_file: File): Promise<void> {
        return Promise.reject(
            new Error(
                'NativeAnyfsDisk: attach(File) not supported in native mode; ' +
                    'use attachPath(string) or fall back to the wasm worker.',
            ),
        );
    }

    /** Open a remote disk image URL. The main process starts an HTTP proxy
     *  server that translates QEMU Range requests into upstream HTTPS
     *  fetches — no TLS/OpenSSL in QEMU, no aio-win32 event loop issues. */
    async attachUrl(url: string, _name?: string): Promise<void> {
        if (this.handle >= 0) throw new Error('attachUrl: already attached');
        const { proxyUrl, id } = await this.bridge.registerUrl(url);
        this.proxyId = id;
        try {
            const h = await this.chain(() => this.bridge.diskOpen(proxyUrl, 1));
            if (h < 0) throw new Error(`diskOpen(${proxyUrl}) failed: rc=${h}`);
            this.handle = h;
        } catch (err) {
            await this.bridge.unregisterUrl(id);
            this.proxyId = null;
            throw err;
        }
    }

    /** No-op — native ops don't emit progress. Returns the no-op
     *  unsubscriber so callers' tear-down is uniform. */
    onProgress(_cb: (step: string) => void): () => void {
        return () => undefined;
    }

    listPartitions(): Promise<PartInfo[]> {
        return this.chain(async () => JSON.parse(await this.bridge.diskListJson(this.handle)));
    }

    diskMeta(): Promise<DiskMeta> {
        return this.chain(async () => JSON.parse(await this.bridge.diskMetaJson(this.handle)));
    }

    enter(part: number, opts: EnterOpts = {}): Promise<string> {
        return this.chain(() => this.bridge.diskEnter(this.handle, part, opts.flags ?? 1));
    }

    mountWhole(fstype?: string): Promise<string> {
        return this.chain(() => this.bridge.mountWhole(this.handle, fstype ?? '', 1));
    }

    readdir(path: string): Promise<DirEntry[]> {
        return this.chain(async () => JSON.parse(await this.bridge.readdirJson(path)));
    }

    stat(path: string): Promise<Stat> {
        return this.chain(async () => JSON.parse(await this.bridge.lstatJson(path)));
    }

    statFollow(path: string): Promise<Stat> {
        return this.chain(async () => JSON.parse(await this.bridge.statJson(path)));
    }

    realpath(path: string): Promise<string> {
        return this.chain(() => this.bridge.realpath(path));
    }

    readlink(path: string): Promise<string> {
        return this.chain(() => this.bridge.readlink(path));
    }

    async open(path: string): Promise<LklFd> {
        const fd = await this.chain(() => this.bridge.fileOpen(path, 0));
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        this.fds.add(fd);
        return fd;
    }

    async read(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        const { rc, data } = await this.chain(() => this.bridge.pread(fd, length, offset));
        if (rc < 0) throw new Error(`pread rc=${rc}`);
        return data;
    }

    async close(fd: LklFd): Promise<void> {
        const rc = await this.chain(() => this.bridge.fileClose(fd));
        this.fds.delete(fd);
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    /** Read a small text file from the in-kernel namespace; mirrors
     *  WorkerAnyfsDisk.readKernelFile so callers can switch backends. */
    async readKernelFile(path: string, maxBytes = 64 * 1024): Promise<string> {
        const fd = await this.open(path);
        try {
            const chunks: Uint8Array[] = [];
            let offset = 0;
            for (let i = 0; i < 64 && offset < maxBytes; i++) {
                const want = Math.min(8192, maxBytes - offset);
                const chunk = await this.read(fd, offset, want);
                if (chunk.length === 0) break;
                chunks.push(chunk);
                offset += chunk.length;
            }
            let total = 0;
            for (const c of chunks) total += c.length;
            const buf = new Uint8Array(total);
            let p = 0;
            for (const c of chunks) {
                buf.set(c, p);
                p += c.length;
            }
            return new TextDecoder('utf-8').decode(buf);
        } finally {
            try {
                await this.close(fd);
            } catch {
                /* best effort */
            }
        }
    }

    async openReadable(
        path: string,
        opts: { chunkSize?: number } = {},
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }> {
        const chunkSize = opts.chunkSize ?? 1024 * 1024;
        const st = await this.statFollow(path);
        const total = st.size;
        const fd = await this.open(path);
        let offset = 0;
        let closed = false;
        const closeFd = async () => {
            if (closed) return;
            closed = true;
            try {
                await this.close(fd);
            } catch {
                /* best effort */
            }
        };
        const self = this;
        const stream = new ReadableStream<Uint8Array>({
            async pull(controller) {
                if (offset >= total) {
                    await closeFd();
                    controller.close();
                    return;
                }
                const want = Math.min(chunkSize, total - offset);
                try {
                    const chunk = await self.read(fd, offset, want);
                    if (chunk.length === 0) {
                        await closeFd();
                        controller.close();
                        return;
                    }
                    offset += chunk.length;
                    controller.enqueue(chunk);
                    if (offset >= total) {
                        await closeFd();
                        controller.close();
                    }
                } catch (err) {
                    await closeFd();
                    controller.error(err);
                }
            },
            async cancel() {
                await closeFd();
            },
        });
        return { stream, size: total };
    }

    async *walk(root: string, chunkSize = 1000): AsyncGenerator<string[]> {
        const queue: string[] = [root];
        let chunk: string[] = [];
        while (queue.length) {
            const dir = queue.shift()!;
            let entries: DirEntry[];
            try {
                entries = await this.readdir(dir);
            } catch {
                continue;
            }
            for (const e of entries) {
                const p = dir === '/' ? `/${e.name}` : `${dir}/${e.name}`;
                chunk.push(p);
                if (e.kind === 'dir') queue.push(p);
                if (chunk.length >= chunkSize) {
                    yield chunk;
                    chunk = [];
                }
            }
        }
        if (chunk.length) yield chunk;
    }

    get _diskHandle(): DiskHandle | -1 {
        return this.handle;
    }

    async dispose(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        // Wait for any in-flight op to settle before closing the disk so we
        // don't get diskClose racing a readdir on the same handle.
        try {
            await this.opChain;
        } catch {
            /* the chain is already swallowing errors; just be defensive */
        }
        if (this.handle >= 0) {
            try {
                await this.bridge.diskClose(this.handle);
            } catch {
                /* best effort — the addon's kernel stays up either way */
            }
            this.handle = -1;
        }
        if (this.proxyId) {
            try {
                await this.bridge.unregisterUrl(this.proxyId);
            } catch {
                /* best effort */
            }
            this.proxyId = null;
        }
        // We deliberately do NOT call kernelHalt — the addon's kernel is
        // process-global and shared with any other mounts the renderer may
        // still hold. The kernel only goes away when the Electron main
        // process exits.
        this.fds.clear();
    }
}
