import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import { AnyfsSessionBase } from './session-base.js';

type Pending = { res: (v: unknown) => void; rej: (e: Error) => void };

/**
 * Browser / Electron wasm session — communicates with a Web Worker running
 * the emscripten LKL bundle via postMessage.
 */
export class WasmSession extends AnyfsSessionBase {
    private readonly worker: Worker;
    private nextId = 1;
    private readonly pending = new Map<number, Pending>();
    private workerError: Error | null = null;

    /** @internal — use mountFile() or prewarm() in index.ts. */
    constructor(worker: Worker) {
        super();
        this.worker = worker;
        this.worker.addEventListener('message', this.onMessage);
        this.worker.addEventListener('error', this.onError);
    }

    // ── Worker lifecycle ───────────────────────────────

    /** Wait for the worker to signal `host-ready`. */
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
            // eslint-disable-next-line no-console
            console.log(
                `[${m.event === 'stderr' ? 'anyfs.err' : 'anyfs.out'}] ${(m.message ?? '').replace(
                    /\n$/,
                    '',
                )}`,
            );
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

    private call<T>(op: string, args: unknown = {}): Promise<T> {
        if (this.disposed) return Promise.reject(new Error('AnyfsSession: already disposed'));
        if (this.workerError) return Promise.reject(this.workerError);
        const id = this.nextId++;
        return new Promise<T>((res, rej) => {
            this.pending.set(id, {
                res: res as (v: unknown) => void,
                rej,
            });
            try {
                this.worker.postMessage({ id, op, args });
            } catch (err) {
                this.pending.delete(id);
                rej(err instanceof Error ? err : new Error(String(err)));
            }
        });
    }

    /** @internal — raw call without tracking, used for initial boot. */
    callRaw<T>(op: string, args: unknown = {}): Promise<T> {
        return this.call<T>(op, args);
    }

    // ── Attach ─────────────────────────────────────────

    async attachFile(file: File): Promise<void> {
        await this.call('attach', { file });
    }

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

    async attachPath(_path: string): Promise<void> {
        throw new Error('WasmSession: attachPath not supported in browser wasm mode');
    }

    // ── Partition / mount ──────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        return this.call<string>('enter', { part, flags });
    }

    async listParts(): Promise<SessionPartInfo[]> {
        return this.call<SessionPartInfo[]>('listParts');
    }

    async meta(): Promise<SessionMeta> {
        return this.call<SessionMeta>('meta');
    }

    // ── Filesystem ops ─────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        return this.call<DirEntry[]>('readdir', { path });
    }

    async stat(path: string): Promise<Stat> {
        return this.call<Stat>('stat', { path });
    }

    async statFollow(path: string): Promise<Stat> {
        return this.call<Stat>('statFollow', { path });
    }

    async readlink(path: string): Promise<string> {
        return this.call<string>('readlink', { path });
    }

    async realpath(path: string): Promise<string> {
        return this.call<string>('realpath', { path });
    }

    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        return this.call<string>('readKernelFile', { path });
    }

    onProgress(cb: (step: string) => void): () => void {
        const handler = (e: MessageEvent) => {
            const m = e.data as { event?: string; step?: string };
            if (m.event === 'progress' && m.step) cb(m.step);
        };
        this.worker.addEventListener('message', handler);
        return () => this.worker.removeEventListener('message', handler);
    }

    // ── Internal fd ops ────────────────────────────────

    /** @internal */
    protected async _openFdRaw(path: string): Promise<LklFd> {
        return this.call<number>('open', { path });
    }

    /** @internal */
    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        return this.call<Uint8Array>('read', { fd, offset, length });
    }

    /** @internal */
    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        await this.call<number>('close', { fd });
    }

    /** @internal */
    protected async _dispose(): Promise<void> {
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
