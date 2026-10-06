import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import type { AnyfsSession } from './session.js';

/** Default per-op watchdog (ms) — see SessionOpts.opTimeoutMs. 0, a negative
 *  value, NaN or Infinity disables the watchdog. */
export const DEFAULT_OP_TIMEOUT_MS = 60_000;

/** Options every session constructor takes. */
export interface SessionBaseOpts {
    /** Per-op watchdog (ms). 0, a negative value, NaN or Infinity disables it;
     *  values above 2^31-1 are capped. Default DEFAULT_OP_TIMEOUT_MS. */
    opTimeoutMs?: number | undefined;
}

/** The engine stopped answering (the op watchdog fired) or reported itself
 *  dead. The session is unusable: only a new worker — or, for the native
 *  addon, a new process — recovers. */
export class EngineFatalError extends Error {
    override name = 'EngineFatalError';
}

/**
 * Abstract base for all session implementations.
 * Subclasses implement the transport-specific abstract methods.
 * The base provides openReadable(), walk(), fd tracking, and close() lifecycle.
 */
export abstract class AnyfsSessionBase implements AnyfsSession {
    protected disposed = false;
    protected readonly fds = new Set<LklFd>();
    /** Normalized per-op watchdog (ms); 0 = off (also for negative/NaN/Infinity). */
    protected readonly opTimeoutMs: number;
    private readonly fatalCbs = new Set<(e: Error) => void>();
    private fatalErr: Error | null = null;
    /** Reject callbacks of pending guarded ops, failed when the session goes fatal. */
    private readonly inflight = new Set<(e: Error) => void>();

    constructor(opts: SessionBaseOpts = {}) {
        const ms = opts.opTimeoutMs ?? DEFAULT_OP_TIMEOUT_MS;
        // setTimeout clamps delays past 2^31-1 ms to ~1 ms; Infinity means "never".
        this.opTimeoutMs = Number.isFinite(ms) && ms > 0 ? Math.min(ms, 2 ** 31 - 1) : 0;
    }

    // ── Subclass contract ─────────────────────────────

    abstract attachBlob(blob: Blob): Promise<void>;
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
        // Best-effort close all tracked fds — but not on a dead engine: it
        // never answers, so waiting on it would hang close() forever.
        if (!this.fatalErr) {
            for (const fd of this.fds) {
                try {
                    await this._closeFdRaw(fd);
                } catch {
                    /* best effort */
                }
            }
        }
        this.fds.clear();
        await this._dispose();
    }

    // ── Fatal-error signalling ────────────────────────

    onFatal(cb: (err: Error) => void): () => void {
        if (this.fatalErr) {
            cb(this.fatalErr);
            return () => {};
        }
        this.fatalCbs.add(cb);
        return () => this.fatalCbs.delete(cb);
    }

    /** @internal — subclasses call this once the session is unrecoverable.
     *  Idempotent: only the first call fires the callbacks. */
    protected fireFatal(err: Error): void {
        if (this.fatalErr) return;
        this.fatalErr = err;
        for (const fail of [...this.inflight]) fail(err);
        for (const cb of this.fatalCbs) {
            try {
                cb(err);
            } catch {
                /* a listener throwing must not block the others */
            }
        }
        this.fatalCbs.clear();
    }

    /** @internal — the error the session died with, or null while healthy. */
    protected get fatalError(): Error | null {
        return this.fatalErr;
    }

    /** @internal — run one engine op under the watchdog. A fatal session
     *  rejects at once instead of queueing work behind a wedged engine. An
     *  op that outlives opTimeoutMs rejects with an EngineFatalError and the
     *  session fires onFatal with the same error: a wedged kernel never
     *  answers again, so the UI must stop waiting for it. */
    protected guard<T>(op: string, run: () => Promise<T>): Promise<T> {
        if (this.fatalErr) return Promise.reject(this.fatalErr);
        const ms = this.opTimeoutMs;
        return new Promise<T>((resolve, reject) => {
            let timer: ReturnType<typeof setTimeout> | undefined;
            const fail = (e: Error): void => {
                if (timer !== undefined) clearTimeout(timer);
                this.inflight.delete(fail);
                reject(e);
            };
            this.inflight.add(fail);
            if (ms > 0) {
                timer = setTimeout(() => {
                    const err = new EngineFatalError(
                        `${op} timed out after ${ms / 1000}s — the engine is wedged`,
                    );
                    // Fatal first, so listeners know the session is dead before
                    // the op's caller sees the rejection.
                    this.fireFatal(err);
                    fail(err);
                }, ms);
            }
            let p: Promise<T>;
            try {
                p = Promise.resolve(run());
            } catch (e) {
                fail(e as Error);
                return;
            }
            p.then(
                (v) => {
                    if (timer !== undefined) clearTimeout(timer);
                    this.inflight.delete(fail);
                    resolve(v);
                },
                (e: unknown) => fail(e as Error),
            );
        });
    }

    // ── Internal ──────────────────────────────────────

    protected check(): void {
        if (this.disposed) throw new Error('AnyfsSession: already disposed');
    }
}
