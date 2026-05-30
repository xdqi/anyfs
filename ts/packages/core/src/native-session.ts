import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import { AnyfsSessionBase } from './session-base.js';

/** Shape of the preload-injected bridge. Async because IPC is async. */
export interface AnyfsNativeBridge {
    available(): Promise<boolean>;
    init(memMb: number, loglevel: number): Promise<number>;
    diskOpen(path: string, flags: number): Promise<number>;
    diskClose(h: number): Promise<number>;
    diskListJson(h: number): Promise<string>;
    diskMetaJson(h: number): Promise<string>;
    diskEnter(h: number, part: number, flags: number): Promise<string>;
    // mountWhole was deleted from the addon — whole-disk is now diskEnter(h, 0, flags)
    readdirJson(path: string): Promise<string>;
    lstatJson(path: string): Promise<string>;
    statJson(path: string): Promise<string>;
    realpath(path: string): Promise<string>;
    readlink(path: string): Promise<string>;
    startProxy(payload: {
        upstreamUrl?: string;
        localPath?: string;
    }): Promise<{ proxyUrl: string; id: string }>;
    stopProxy(id: string): Promise<void>;
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

/**
 * Electron native-addon session — communicates via the preload-injected
 * `window.anyfsNative` IPC bridge.
 *
 * The host kernel is process-global and idempotently booted via `boot()`.
 */
export class NativeSession extends AnyfsSessionBase {
    private readonly bridge: AnyfsNativeBridge;
    private handle = -1;
    private proxyId: string | null = null;
    // Serialize ops so a slow readdir doesn't interleave with a pread on
    // the same kernel.
    private opChain: Promise<unknown> = Promise.resolve();

    constructor(bridge: AnyfsNativeBridge) {
        super();
        this.bridge = bridge;
    }

    // ── Boot (platform-specific, not on AnyfsSession) ──

    /** Boot the addon's kernel (idempotent in the main process). */
    async boot(memMb: number, loglevel: number): Promise<void> {
        const rc = await this.bridge.init(memMb, loglevel);
        if (rc !== 0) throw new Error(`anyfs-native init failed: rc=${rc}`);
    }

    // ── Op serialization ───────────────────────────────

    private chain<T>(fn: () => Promise<T>): Promise<T> {
        if (this.disposed) return Promise.reject(new Error('AnyfsSession: already disposed'));
        const next = this.opChain.then(fn, fn);
        this.opChain = next.catch(() => undefined);
        return next;
    }

    // ── Attach ─────────────────────────────────────────

    async attachPath(path: string): Promise<void> {
        if (this.handle >= 0) throw new Error('NativeSession: already attached');
        const h = await this.chain(() => this.bridge.diskOpen(path, 1));
        if (h < 0) throw new Error(`diskOpen failed: rc=${h}`);
        this.handle = h;
    }

    async attachBlob(_blob: Blob): Promise<void> {
        throw new Error(
            'NativeSession: attachBlob(Blob) not supported in native mode; use attachPath(string) or fall back to the wasm worker.',
        );
    }

    async attachUrl(url: string, _name?: string): Promise<void> {
        if (this.handle >= 0) throw new Error('NativeSession: already attached');
        const { proxyUrl, id } = await this.bridge.startProxy({ upstreamUrl: url });
        this.proxyId = id;
        try {
            const h = await this.chain(() => this.bridge.diskOpen(proxyUrl, 1));
            if (h < 0) throw new Error(`diskOpen(${proxyUrl}) failed: rc=${h}`);
            this.handle = h;
        } catch (err) {
            await this.bridge.stopProxy(id);
            this.proxyId = null;
            throw err;
        }
    }

    // ── Partition / mount ──────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        return this.chain(() => this.bridge.diskEnter(this.handle, part, flags));
    }

    async listParts(): Promise<SessionPartInfo[]> {
        return this.chain(async () => JSON.parse(await this.bridge.diskListJson(this.handle)));
    }

    async meta(): Promise<SessionMeta> {
        return this.chain(async () => JSON.parse(await this.bridge.diskMetaJson(this.handle)));
    }

    // ── Filesystem ops ─────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        return this.chain(async () => JSON.parse(await this.bridge.readdirJson(path)));
    }

    async stat(path: string): Promise<Stat> {
        return this.chain(async () => JSON.parse(await this.bridge.lstatJson(path)));
    }

    async statFollow(path: string): Promise<Stat> {
        return this.chain(async () => JSON.parse(await this.bridge.statJson(path)));
    }

    async readlink(path: string): Promise<string> {
        return this.chain(() => this.bridge.readlink(path));
    }

    async realpath(path: string): Promise<string> {
        return this.chain(() => this.bridge.realpath(path));
    }

    async readKernelFile(path: string, maxBytes = 64 * 1024): Promise<string> {
        const fd = await this.openFd(path);
        try {
            const chunks: Uint8Array[] = [];
            let offset = 0;
            for (let i = 0; i < 64 && offset < maxBytes; i++) {
                const want = Math.min(8192, maxBytes - offset);
                const chunk = await this.readFd(fd, offset, want);
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
                await this.closeFd(fd);
            } catch {
                /* best effort */
            }
        }
    }

    onProgress(_cb: (step: string) => void): () => void {
        // Native ops don't emit progress.
        return () => undefined;
    }

    // ── Internal fd ops ────────────────────────────────

    /** @internal */
    protected async _openFdRaw(path: string): Promise<LklFd> {
        const fd = await this.chain(() => this.bridge.fileOpen(path, 0));
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        return fd;
    }

    /** @internal */
    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        const { rc, data } = await this.chain(() => this.bridge.pread(fd, length, offset));
        if (rc < 0) throw new Error(`pread rc=${rc}`);
        return data;
    }

    /** @internal */
    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        const rc = await this.chain(() => this.bridge.fileClose(fd));
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    /** @internal */
    protected async _dispose(): Promise<void> {
        // Wait for any in-flight op to settle.
        try {
            await this.opChain;
        } catch {
            /* the chain is already swallowing errors */
        }
        if (this.handle >= 0) {
            try {
                await this.bridge.diskClose(this.handle);
            } catch {
                /* best effort */
            }
            this.handle = -1;
        }
        if (this.proxyId) {
            try {
                await this.bridge.stopProxy(this.proxyId);
            } catch {
                /* best effort */
            }
            this.proxyId = null;
        }
        // Deliberately do NOT call kernelHalt — the addon's kernel is
        // process-global and shared with other mounts.
    }
}
