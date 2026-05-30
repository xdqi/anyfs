import type { AnyfsModule } from './module.js';
import type { DirEntry, LklFd, SessionMeta, SessionPartInfo, Stat } from './types.js';
import { AnyfsSessionBase } from './session-base.js';

const JSON_BUF_INIT = 4096;

/**
 * Node wasm session — direct emscripten ccall (no worker, no ASYNCIFY).
 *
 * The caller owns kernel boot (bootModule) and passes the live module to the
 * constructor. `attachPath(fsPath)` opens the disk image via the C glue;
 * NODEFS mount of the host directory must already be set up through boot's
 * `preRun` hook.
 */
export class NodeWasmSession extends AnyfsSessionBase {
    private readonly M: AnyfsModule;
    private handle = -1;

    /** @internal — use mountNodeFile() in node.ts or construct directly. */
    constructor(M: AnyfsModule) {
        super();
        this.M = M;
    }

    // ── JSON-overflow helpers ──────────────────────────

    private callJsonPath(fnName: string, path: string): string {
        let cap = JSON_BUF_INIT;
        while (true) {
            const buf = this.M._malloc(cap);
            try {
                const ret = this.M.ccall(
                    fnName,
                    'number',
                    ['string', 'number', 'number'],
                    [path, buf, cap],
                ) as number;
                if (ret < 0) {
                    cap = Math.max(cap * 2, -ret);
                    continue;
                }
                return this.M.UTF8ToString(buf, ret);
            } finally {
                this.M._free(buf);
            }
        }
    }

    private callJsonHandle(fnName: string): string {
        let cap = JSON_BUF_INIT;
        while (true) {
            const buf = this.M._malloc(cap);
            try {
                const ret = this.M.ccall(
                    fnName,
                    'number',
                    ['number', 'number', 'number'],
                    [this.handle, buf, cap],
                ) as number;
                if (ret < 0) {
                    cap = Math.max(cap * 2, -ret);
                    continue;
                }
                return this.M.UTF8ToString(buf, ret);
            } finally {
                this.M._free(buf);
            }
        }
    }

    // ── Attach ─────────────────────────────────────────

    async attachPath(fsPath: string): Promise<void> {
        this.check();
        if (this.handle >= 0) throw new Error('NodeWasmSession: already attached');
        const h = this.M.ccall(
            'anyfs_ts_session_open',
            'number',
            ['string', 'number'],
            [fsPath, 0],
        ) as number;
        if (h < 0) throw new Error(`session_open(${fsPath}) failed: ${h}`);
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
        const cap = 128;
        const buf = this.M._malloc(cap);
        try {
            const rc = this.M.ccall(
                'anyfs_ts_session_enter',
                'number',
                ['number', 'number', 'number', 'number', 'number'],
                [this.handle, part, flags, buf, cap],
            ) as number;
            if (rc !== 0) throw new Error(`session_enter failed: rc=${rc}`);
            return this.M.UTF8ToString(buf);
        } finally {
            this.M._free(buf);
        }
    }

    async listParts(): Promise<SessionPartInfo[]> {
        this.check();
        const json = this.callJsonHandle('anyfs_ts_session_list_json');
        return JSON.parse(json) as SessionPartInfo[];
    }

    async meta(): Promise<SessionMeta> {
        this.check();
        const json = this.callJsonHandle('anyfs_ts_session_meta_json');
        return JSON.parse(json) as SessionMeta;
    }

    // ── Filesystem ops ─────────────────────────────────

    async readdir(path: string): Promise<DirEntry[]> {
        this.check();
        const json = this.callJsonPath('anyfs_ts_readdir_json', path);
        return JSON.parse(json) as DirEntry[];
    }

    async stat(path: string): Promise<Stat> {
        this.check();
        const json = this.callJsonPath('anyfs_ts_lstat_json', path);
        return JSON.parse(json) as Stat;
    }

    async statFollow(path: string): Promise<Stat> {
        this.check();
        const json = this.callJsonPath('anyfs_ts_stat_json', path);
        return JSON.parse(json) as Stat;
    }

    async readlink(path: string): Promise<string> {
        this.check();
        const cap = 4096;
        const buf = this.M._malloc(cap);
        try {
            const n = this.M.ccall(
                'anyfs_ts_readlink',
                'number',
                ['string', 'number', 'number'],
                [path, buf, cap],
            ) as number;
            if (n < 0) throw new Error(`readlink rc=${n}`);
            return this.M.UTF8ToString(buf, n);
        } finally {
            this.M._free(buf);
        }
    }

    async realpath(path: string): Promise<string> {
        this.check();
        const cap = 4096;
        const buf = this.M._malloc(cap);
        try {
            const n = this.M.ccall(
                'anyfs_ts_realpath',
                'number',
                ['string', 'number', 'number'],
                [path, buf, cap],
            ) as number;
            if (n < 0) throw new Error(`realpath rc=${n}`);
            return this.M.UTF8ToString(buf, n);
        } finally {
            this.M._free(buf);
        }
    }

    async readKernelFile(path: string, _maxBytes?: number): Promise<string> {
        this.check();
        let cap = 4096;
        for (let i = 0; i < 5; i++) {
            const buf = this.M._malloc(cap);
            try {
                const n = this.M.ccall(
                    'anyfs_ts_read_kernel_file',
                    'number',
                    ['string', 'number', 'number'],
                    [path, buf, cap],
                ) as number;
                if (n >= 0) return this.M.UTF8ToString(buf, n);
                const need = -n;
                if (need <= cap) throw new Error(`readKernelFile rc=${n}`);
                cap = Math.max(need + 256, cap * 2);
            } finally {
                this.M._free(buf);
            }
        }
        throw new Error('readKernelFile: buffer too large');
    }

    onProgress(_cb: (step: string) => void): () => void {
        // Direct ccall has no progress events — return a no-op unsubscriber.
        return () => undefined;
    }

    // ── Internal fd ops ────────────────────────────────

    /** @internal */
    protected async _openFdRaw(path: string): Promise<LklFd> {
        const fd = this.M.ccall(
            'anyfs_ts_open',
            'number',
            ['string', 'number'],
            [path, 0],
        ) as number;
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        return fd;
    }

    /** @internal */
    protected async _readFdRaw(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        const buf = this.M._malloc(length);
        try {
            const got = this.M.ccall(
                'anyfs_ts_pread',
                'number',
                ['number', 'number', 'number', 'bigint'],
                [fd, buf, length, BigInt(offset)],
            ) as unknown as bigint | number;
            const n = typeof got === 'bigint' ? Number(got) : got;
            if (n < 0) throw new Error(`pread failed: ${n}`);
            return new Uint8Array(this.M.HEAPU8.buffer, buf, n).slice();
        } finally {
            this.M._free(buf);
        }
    }

    /** @internal */
    protected async _closeFdRaw(fd: LklFd): Promise<void> {
        const rc = this.M.ccall('anyfs_ts_close', 'number', ['number'], [fd]) as number;
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    /** @internal */
    protected async _dispose(): Promise<void> {
        if (this.handle >= 0) {
            try {
                this.M.ccall('anyfs_ts_session_close', 'number', ['number'], [this.handle]);
            } catch {
                /* best effort */
            }
            this.handle = -1;
        }
    }
}
