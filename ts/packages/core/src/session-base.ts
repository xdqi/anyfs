import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import type { AnyfsSession } from './session.js';

/**
 * Abstract base for all session implementations.
 * Subclasses implement the transport-specific abstract methods.
 * The base provides openReadable(), walk(), fd tracking, and close() lifecycle.
 */
export abstract class AnyfsSessionBase implements AnyfsSession {
    protected disposed = false;
    protected readonly fds = new Set<LklFd>();

    // ── Subclass contract ─────────────────────────────

    abstract attachFile(file: File): Promise<void>;
    abstract attachUrl(url: string, name?: string): Promise<void>;
    abstract attachPath(path: string): Promise<void>;
    abstract enter(part: number, flags?: number): Promise<string>;
    abstract listParts(): Promise<SessionPartInfo[]>;
    abstract meta(): Promise<SessionMeta>;
    abstract readdir(path: string): Promise<DirEntry[]>;
    abstract stat(path: string): Promise<Stat>;
    abstract statFollow(path: string): Promise<Stat>;
    abstract readlink(path: string): Promise<string>;
    abstract realpath(path: string): Promise<string>;
    abstract readKernelFile(path: string, maxBytes?: number): Promise<string>;
    abstract onProgress(cb: (step: string) => void): () => void;

    /** @internal — open a file descriptor (transport-specific). */
    protected abstract _openFdRaw(path: string): Promise<LklFd>;
    /** @internal — read from a file descriptor (transport-specific). */
    protected abstract _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array>;
    /** @internal — close a file descriptor (transport-specific). */
    protected abstract _closeFdRaw(fd: LklFd): Promise<void>;
    /** @internal — release backend resources (transport-specific). */
    protected abstract _dispose(): Promise<void>;

    // ── Public fd ops (with tracking) ─────────────────

    async openFd(path: string): Promise<LklFd> {
        this.check();
        const fd = await this._openFdRaw(path);
        this.fds.add(fd);
        return fd;
    }

    async readFd(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        this.check();
        return this._readFdRaw(fd, offset, length);
    }

    async closeFd(fd: LklFd): Promise<void> {
        this.check();
        this.fds.delete(fd);
        await this._closeFdRaw(fd);
    }

    // ── Shared: openReadable ──────────────────────────

    async openReadable(
        path: string,
        opts: { chunkSize?: number } = {},
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }> {
        const chunkSize = opts.chunkSize ?? 1024 * 1024;
        const st = await this.statFollow(path);
        const total = st.size;
        const fd = await this.openFd(path);
        let offset = 0;
        let closed = false;
        const closeFd = async () => {
            if (closed) return;
            closed = true;
            try {
                await this.closeFd(fd);
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
                    const chunk = await self.readFd(fd, offset, want);
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

    // ── Shared: walk ──────────────────────────────────

    async *walk(root: string, chunkSize = 1000): AsyncGenerator<string[]> {
        this.check();
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

    // ── Shared: close lifecycle ───────────────────────

    async close(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        // Best-effort close all tracked fds
        for (const fd of this.fds) {
            try {
                await this._closeFdRaw(fd);
            } catch {
                /* best effort */
            }
        }
        this.fds.clear();
        await this._dispose();
    }

    // ── Internal ──────────────────────────────────────

    protected check(): void {
        if (this.disposed) throw new Error('AnyfsSession: already disposed');
    }
}
