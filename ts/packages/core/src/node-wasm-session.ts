import type { AnyfsModule } from './module.js';
import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import { AnyfsSessionBase, type SessionBaseOpts } from './session-base.js';
import { ApiOp, wasmApiFor, type WasmApi } from './wasm-api.js';

export interface NodeWasmSessionOpts extends SessionBaseOpts {
    /** Open the image read-only (ANYFS_SESSION_READONLY), as the browser
     *  worker does. Default false. */
    readOnly?: boolean | undefined;
}

/**
 * Node wasm session — the bundle runs on Node's main thread (no Worker).
 *
 * Every op goes through the bundle's API thread (see wasm-api.ts): this
 * thread owns the module and must stay free to serve the NODEFS calls the
 * QEMU thread proxies to it.
 *
 * The caller owns kernel boot (bootModule) and passes the live module to the
 * constructor. `attachPath(fsPath)` opens the disk image via the C glue;
 * NODEFS mount of the host directory must already be set up through boot's
 * `preRun` hook.
 *
 * The module is process-global. When it aborts (kernel panic, wasm trap)
 * every session on it fires onFatal, and recovery means a new process.
 */
export class NodeWasmSession extends AnyfsSessionBase {
    private readonly M: AnyfsModule;
    private readonly api: WasmApi;
    private readonly openFlags: number;
    private readonly unsubscribeFail: () => void;
    private handle = -1;

    /** @internal — use mountNodeFile() in node.ts or construct directly. */
    constructor(M: AnyfsModule, opts: NodeWasmSessionOpts = {}) {
        super(opts);
        this.M = M;
        this.api = wasmApiFor(M);
        this.openFlags = opts.readOnly ? 1 : 0;
        this.unsubscribeFail = this.api.onFail((err) => this.fireFatal(err));
    }

    // ── Attach ─────────────────────────────────────────

    async attachPath(fsPath: string): Promise<void> {
        this.check();
        if (this.handle >= 0) throw new Error('NodeWasmSession: already attached');
        const h = await this.api.call(ApiOp.SESSION_OPEN, [fsPath, this.openFlags]);
        if (h < 0) {
            const why = await this.api.lastError();
            throw new Error(`session_open(${fsPath}) failed: ${why || `rc=${h}`}`);
        }
        this.handle = h;
    }

    async attachBlob(_blob: Blob): Promise<void> {
        throw new Error('NodeWasmSession: attachBlob(Blob) not supported; use attachPath(string)');
    }

    async attachUrl(_url: string, _name?: string): Promise<void> {
        throw new Error('NodeWasmSession: attachUrl not supported in Node wasm mode');
    }

    // ── Partition / mount ──────────────────────────────

    async enter(part: number, flags = 0): Promise<string> {
        this.check();
        return this.guard('enter', async () => {
            const cap = 128;
            const buf = this.M._malloc(cap);
            try {
                const rc = await this.api.call(ApiOp.SESSION_ENTER, [
                    this.handle,
                    part,
                    flags,
                    buf,
                    cap,
                ]);
                if (rc !== 0) {
                    const why = await this.api.lastError();
                    throw new Error(`session_enter failed: rc=${rc}${why ? `: ${why}` : ''}`);
                }
                return this.M.UTF8ToString(buf);
            } finally {
                this.api.free(buf);
            }
        });
    }

    async listParts(): Promise<SessionPartInfo[]> {
        this.check();
        return this.guard(
            'listParts',
            async () =>
                (await this.api.callJsonOut(
                    ApiOp.SESSION_LIST,
                    [this.handle],
                    'session_list_json',
                )) as SessionPartInfo[],
        );
    }

    async meta(): Promise<SessionMeta> {
        this.check();
        return this.guard(
            'meta',
            async () =>
                (await this.api.callJsonOut(
                    ApiOp.SESSION_META,
                    [this.handle],
                    'session_meta_json',
                )) as SessionMeta,
        );
    }

    // ── Filesystem ops ─────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        this.check();
        return this.guard(
            'readdir',
            async () =>
                (await this.api.callJsonOut(ApiOp.READDIR, [path], 'readdir_json')) as DirEntry[],
        );
    }

    async stat(path: string): Promise<Stat> {
        this.check();
        return this.guard(
            'stat',
            async () => (await this.api.callJsonOut(ApiOp.LSTAT, [path], 'lstat_json')) as Stat,
        );
    }

    async statFollow(path: string): Promise<Stat> {
        this.check();
        return this.guard(
            'statFollow',
            async () => (await this.api.callJsonOut(ApiOp.STAT, [path], 'stat_json')) as Stat,
        );
    }

    async readlink(path: string): Promise<string> {
        this.check();
        return this.guard('readlink', () => this.pathOut(ApiOp.READLINK, path, 'readlink'));
    }

    async realpath(path: string): Promise<string> {
        this.check();
        return this.guard('realpath', () => this.pathOut(ApiOp.REALPATH, path, 'realpath'));
    }

    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        this.check();
        return this.guard('readKernelFile', () =>
            this.api.callStringOut(ApiOp.READ_KERNEL_FILE, [path], 'readKernelFile', 4096),
        );
    }

    onProgress(_cb: (step: string) => void): () => void {
        // Direct calls have no progress events — return a no-op unsubscriber.
        return () => undefined;
    }

    // ── Internal fd ops ────────────────────────────────

    /** @internal */
    protected async _openFdRaw(path: string): Promise<LklFd> {
        return this.guard('open', async () => {
            const fd = await this.api.call(ApiOp.OPEN, [path, 0]);
            if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
            return fd;
        });
    }

    /** @internal */
    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        return this.guard('read', async () => {
            const buf = this.M._malloc(length);
            try {
                const off = BigInt(offset);
                const lo = Number(off & 0xffffffffn) | 0;
                const hi = Number((off >> 32n) & 0xffffffffn) | 0;
                const n = await this.api.call(ApiOp.PREAD, [fd, buf, length, lo, hi]);
                if (n < 0) throw new Error(`pread failed: ${n}`);
                return this.M.HEAPU8.slice(buf, buf + n);
            } finally {
                this.api.free(buf);
            }
        });
    }

    /** @internal */
    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        return this.guard('close', async () => {
            const rc = await this.api.call(ApiOp.CLOSE, [fd]);
            if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
        });
    }

    /** @internal */
    protected async _dispose(): Promise<void> {
        this.unsubscribeFail();
        // A wedged or aborted engine never answers session_close.
        if (this.handle >= 0 && !this.fatalError) {
            try {
                await this.api.call(ApiOp.SESSION_CLOSE, [this.handle]);
            } catch {
                /* best effort */
            }
        }
        this.handle = -1;
    }

    // PATH_MAX is 4096 on Linux; longer can't be represented anyway.
    private async pathOut(op: number, path: string, name: string): Promise<string> {
        const cap = 4096;
        const buf = this.M._malloc(cap);
        try {
            const n = await this.api.call(op, [path, buf, cap]);
            if (n < 0) throw new Error(`${name} rc=${n}`);
            return this.M.UTF8ToString(buf, n);
        } finally {
            this.api.free(buf);
        }
    }
}
