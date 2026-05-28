import type { DirEntry, DiskHandle, DiskMeta, EnterOpts, LklFd, PartInfo, Stat } from './types.js';

type Pending = { res: (v: unknown) => void; rej: (e: Error) => void };

/**
 * What the AnyfsProvider can attach to.
 *  - `file`: a local `File`/`Blob` mounted via emscripten's WORKERFS. Only
 *    the wasm worker path supports it — native rejects File.
 *  - `url`:  a remote HTTP image; wasm uses URLFS (sync XHR + LRU range
 *    cache), native uses QEMU's curl block driver via `attachPath(url)`.
 *  - `path`: an absolute host filesystem path or block device. Native only
 *    (Electron showOpenDialog / system-drive picker). Browser rejects.
 */
export type DiskSource =
    | { kind: 'file'; file: File }
    | { kind: 'url'; url: string; name?: string }
    | { kind: 'path'; path: string; name?: string };

/** Wraps a Worker hosting the wasm anyfs runtime; mirrors the public surface
 *  of the direct-mode AnyfsDisk. Implementation detail: every method posts a
 *  message to the worker and awaits a matching reply. */
export class WorkerAnyfsDisk {
    private readonly worker: Worker;
    private nextId = 1;
    private readonly pending = new Map<number, Pending>();
    private readonly fds = new Set<LklFd>();
    private disposed = false;
    private workerError: Error | null = null;

    /** @internal — use mountFile() in index.ts. */
    constructor(worker: Worker) {
        this.worker = worker;
        this.worker.addEventListener('message', this.onMessage);
        this.worker.addEventListener('error', this.onError);
    }

    private onMessage = (e: MessageEvent) => {
        const m = e.data as {
            id?: number;
            ok?: boolean;
            result?: unknown;
            error?: string;
            stack?: string;
            event?: string;
            message?: string;
            reason?: string;
            step?: string;
        };
        if (m.event === 'abort' || m.event === 'host-error' || m.event === 'host-rejection') {
            this.workerError = new Error(`anyfs worker ${m.event}: ${m.message ?? m.reason ?? ''}`);
            for (const p of this.pending.values()) p.rej(this.workerError);
            this.pending.clear();
            return;
        }
        if (m.event === 'stdout' || m.event === 'stderr') {
            // Surface emscripten print/printErr to the host console so the
            // demo can show LKL/QEMU debug output during development. Lines
            // already arrive newline-terminated; trim trailing whitespace.
            const tag = m.event === 'stderr' ? 'anyfs.err' : 'anyfs.out';
            // eslint-disable-next-line no-console
            console.log(`[${tag}] ${(m.message ?? '').replace(/\n$/, '')}`);
            return;
        }
        if (m.event === 'progress') {
            // eslint-disable-next-line no-console
            console.log(`[anyfs] ${m.step ?? ''}`);
            return;
        }
        if (typeof m.id !== 'number') return;
        const p = this.pending.get(m.id);
        if (!p) return;
        this.pending.delete(m.id);
        if (m.ok) p.res(m.result);
        else p.rej(new Error((m.error ?? 'worker call failed') + (m.stack ? `\n${m.stack}` : '')));
    };

    private onError = (e: ErrorEvent) => {
        this.workerError = new Error(`anyfs worker error: ${e.message}`);
        for (const p of this.pending.values()) p.rej(this.workerError);
        this.pending.clear();
    };

    /** @internal — used by mountFile to wait for `host-ready`. */
    static waitForReady(worker: Worker, timeoutMs = 10000): Promise<void> {
        return new Promise((res, rej) => {
            const t = setTimeout(() => {
                cleanup();
                rej(new Error('anyfs worker did not become ready'));
            }, timeoutMs);
            const onMsg = (e: MessageEvent) => {
                const m = e.data as { event?: string; message?: string };
                if (m.event === 'host-ready') {
                    cleanup();
                    res();
                } else if (m.event === 'host-error' || m.event === 'abort') {
                    cleanup();
                    rej(new Error(`anyfs worker boot ${m.event}: ${m.message ?? ''}`));
                }
            };
            const onErr = (ev: ErrorEvent) => {
                cleanup();
                rej(new Error(`anyfs worker error: ${ev.message}`));
            };
            const cleanup = () => {
                clearTimeout(t);
                worker.removeEventListener('message', onMsg);
                worker.removeEventListener('error', onErr);
            };
            worker.addEventListener('message', onMsg);
            worker.addEventListener('error', onErr);
        });
    }

    private call<T>(op: string, args: unknown = {}): Promise<T> {
        if (this.disposed) return Promise.reject(new Error('AnyfsDisk: already disposed'));
        if (this.workerError) return Promise.reject(this.workerError);
        const id = this.nextId++;
        return new Promise<T>((res, rej) => {
            this.pending.set(id, { res: res as (v: unknown) => void, rej });
            this.worker.postMessage({ id, op, args });
        });
    }

    /** @internal — used by mountFile to issue the initial mount op. */
    callRaw<T>(op: string, args: unknown = {}): Promise<T> {
        return this.call<T>(op, args);
    }

    /** Attach a disk image to a kernel previously booted via `prewarm()`.
     *  After this resolves the disk is ready for listPartitions / mountWhole. */
    async attach(file: File): Promise<void> {
        await this.call('attach', { file });
    }

    /** Attach a remote disk image served over HTTP. The server must honor
     *  `Range:` requests; reads are issued lazily and cached in an LRU. */
    async attachUrl(url: string, name?: string): Promise<void> {
        let fallback = name?.trim() || '';
        if (!fallback) {
            try {
                const u = new URL(
                    url,
                    typeof self !== 'undefined' ? self.location.href : 'http://x/',
                );
                fallback = u.pathname.split('/').filter(Boolean).pop() || 'image';
            } catch {
                fallback = 'image';
            }
        }
        await this.call('attachUrl', { url, name: fallback });
    }

    /** Subscribe to per-step progress events from the worker.
     *  Returns an unsubscribe function. Events fire only during boot/attach. */
    onProgress(cb: (step: string) => void): () => void {
        const handler = (e: MessageEvent) => {
            const m = e.data as { event?: string; step?: string };
            if (m.event === 'progress' && m.step) cb(m.step);
        };
        this.worker.addEventListener('message', handler);
        return () => this.worker.removeEventListener('message', handler);
    }

    async listPartitions(): Promise<PartInfo[]> {
        return this.call<PartInfo[]>('listPartitions');
    }

    async diskMeta(): Promise<DiskMeta> {
        return this.call<DiskMeta>('diskMeta');
    }

    async enter(part: number, opts: EnterOpts = {}): Promise<string> {
        return this.call<string>('enter', { part, flags: opts.flags ?? 1 });
    }

    async mountWhole(fstype?: string): Promise<string> {
        return this.call<string>('mountWhole', { fstype });
    }

    async readdir(path: string): Promise<DirEntry[]> {
        return this.call<DirEntry[]>('readdir', { path });
    }

    async stat(path: string): Promise<Stat> {
        return this.call<Stat>('stat', { path });
    }

    /** Follow-symlinks stat. Used internally by openReadable. */
    async statFollow(path: string): Promise<Stat> {
        return this.call<Stat>('statFollow', { path });
    }

    /** Canonicalize a directory path (all symlink hops resolved). Only
     *  valid when `path` resolves to a directory; rejects with errno text
     *  otherwise. */
    async realpath(path: string): Promise<string> {
        return this.call<string>('realpath', { path });
    }

    /** Read a symlink's stored target string verbatim. Throws if `path` is
     *  not a symlink. */
    async readlink(path: string): Promise<string> {
        return this.call<string>('readlink', { path });
    }

    async open(path: string): Promise<LklFd> {
        const fd = await this.call<number>('open', { path });
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        this.fds.add(fd);
        return fd;
    }

    async read(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        return this.call<Uint8Array>('read', { fd, offset, length });
    }

    async close(fd: LklFd): Promise<void> {
        const rc = await this.call<number>('close', { fd });
        this.fds.delete(fd);
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    /**
     * Read a small text file from the in-kernel namespace (e.g. `/proc/filesystems`,
     * `/proc/mounts`). Does not require an attached disk — only that the kernel
     * has been booted. Delegates to a single native call (open+read+close in C). */
    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        return this.call<string>('readKernelFile', { path });
    }

    /**
     * Open a file as a chunked ReadableStream. Each pull issues a
     * `read(fd, ..., chunkSize)` over the worker. The fd is closed when the
     * stream ends, errors, or is cancelled — so consumers can pipe huge files
     * to `showSaveFilePicker()` without buffering them in memory.
     *
     * Default chunk size 1 MiB; smaller chunks reduce wasm peak memory usage
     * but increase postMessage overhead.
     */
    async openReadable(
        path: string,
        opts: { chunkSize?: number } = {},
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }> {
        const chunkSize = opts.chunkSize ?? 1024 * 1024;
        // Follow symlinks: a bare lstat() on /etc/os-release would report the
        // 21-byte link-target string and truncate the stream there.
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

    /** BFS over the mounted tree; yields chunks of paths (default 1000 each). */
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

    /** @internal — used by index.ts for diskHandle bookkeeping. */
    get _diskHandle(): DiskHandle | -1 {
        return -1;
    }

    async dispose(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        try {
            await this.call<number>('dispose');
        } catch {
            /* best effort */
        }
        this.worker.removeEventListener('message', this.onMessage);
        this.worker.removeEventListener('error', this.onError);
        this.worker.terminate();
        this.pending.clear();
    }
}
