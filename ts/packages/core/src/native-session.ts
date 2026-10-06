import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import { AnyfsSessionBase, EngineFatalError, type SessionBaseOpts } from './session-base.js';

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
    /** Subscribe to fatal engine errors (the addon's QEMU thread missed its
     *  watchdog; pending calls never settle). Returns an unsubscribe fn.
     *  Optional: older hosts don't push these. */
    onFatal?(cb: (reason: string) => void): () => void;
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
    private readonly unsubscribeFatal: (() => void) | null;

    constructor(bridge: AnyfsNativeBridge, opts: SessionBaseOpts = {}) {
        super(opts);
        this.bridge = bridge;
        // The host reports a wedged engine (the QEMU thread missed its own
        // watchdog). Either way, that or our op watchdog, the session is
        // fatal and dispose must not wait on the engine.
        this.unsubscribeFatal =
            bridge.onFatal?.((reason) => {
                this.fireFatal(new EngineFatalError(`anyfs-native engine failed: ${reason}`));
            }) ?? null;
    }

    // ── Boot (platform-specific, not on AnyfsSession) ──

    /** Boot the addon's kernel (idempotent in the main process). */
    async boot(memMb: number, loglevel: number): Promise<void> {
        const rc = await this.bridge.init(memMb, loglevel);
        if (rc !== 0) throw new Error(`anyfs-native init failed: rc=${rc}`);
    }

    // ── Op serialization ───────────────────────────────

    /** An engine op: serialized (a slow readdir must not interleave with a
     *  pread on the same kernel), then run under the base watchdog. The timer
     *  starts when the op leaves this session's queue. It still
     *  includes the IPC hop and any wait on the addon's global op mutex behind
     *  other sessions. */
    private op<T>(op: string, fn: () => Promise<T>): Promise<T> {
        return this.serialize(() => this.guard(op, fn));
    }

    // ── Attach ─────────────────────────────────────────

    async attachPath(path: string): Promise<void> {
        if (this.handle >= 0) throw new Error('NativeSession: already attached');
        const h = await this.serialize(() => this.bridge.diskOpen(path, 1));
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
            const h = await this.serialize(() => this.bridge.diskOpen(proxyUrl, 1));
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
        return this.op('enter', () => this.bridge.diskEnter(this.handle, part, flags));
    }

    async listParts(): Promise<SessionPartInfo[]> {
        return this.op('listParts', async () =>
            JSON.parse(await this.bridge.diskListJson(this.handle)),
        );
    }

    async meta(): Promise<SessionMeta> {
        return this.op('meta', async () => JSON.parse(await this.bridge.diskMetaJson(this.handle)));
    }

    // ── Filesystem ops ─────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        return this.op('readdir', async () => JSON.parse(await this.bridge.readdirJson(path)));
    }

    async stat(path: string): Promise<Stat> {
        return this.op('stat', async () => JSON.parse(await this.bridge.lstatJson(path)));
    }

    async statFollow(path: string): Promise<Stat> {
        return this.op('statFollow', async () => JSON.parse(await this.bridge.statJson(path)));
    }

    async readlink(path: string): Promise<string> {
        return this.op('readlink', () => this.bridge.readlink(path));
    }

    async realpath(path: string): Promise<string> {
        return this.op('realpath', () => this.bridge.realpath(path));
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
        const fd = await this.op('open', () => this.bridge.fileOpen(path, 0));
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        return fd;
    }

    /** @internal */
    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        const { rc, data } = await this.op('read', () => this.bridge.pread(fd, length, offset));
        if (rc < 0) throw new Error(`pread rc=${rc}`);
        return data;
    }

    /** @internal */
    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        const rc = await this.op('close', () => this.bridge.fileClose(fd));
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    /** @internal */
    protected async _dispose(): Promise<void> {
        // A wedged engine never settles the in-flight op or a close call: wait
        // for the queue to drain, but stop waiting as soon as the session goes
        // fatal (watchdog or host report). The bridge subscription stays until
        // the very end so a host fatal during any wait below is seen.
        if (!this.fatalError) await Promise.race([this.queueIdle, this.fatalSignal]);
        // Re-check before every engine call. These go straight to the bridge
        // (not through serialize, which rejects after dispose), but stay
        // bounded by the watchdog.
        if (!this.fatalError) {
            // The addon's kernel is process-global: an fd left open keeps the
            // mount busy, so diskClose would unbind the disk under a live mount.
            for (const fd of this.fds) {
                if (this.fatalError) break;
                await this.guard('close', () => this.bridge.fileClose(fd)).catch(() => {});
            }
        }
        if (this.handle >= 0) {
            if (!this.fatalError) {
                const h = this.handle;
                await this.guard('diskClose', () => this.bridge.diskClose(h)).catch(() => {});
            }
            this.handle = -1;
        }
        // Main-side only, never touches the addon: safe on a dead engine too.
        if (this.proxyId) {
            try {
                await this.bridge.stopProxy(this.proxyId);
            } catch {
                /* best effort */
            }
            this.proxyId = null;
        }
        this.unsubscribeFatal?.();
        // Deliberately do NOT call kernelHalt — the addon's kernel is
        // process-global and shared with other mounts.
    }
}
