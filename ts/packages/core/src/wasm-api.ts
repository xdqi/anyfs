/**
 * Calls into the wasm bundle through its API thread.
 *
 * Every anyfs op runs on one long-lived pthread (ts/native/anyfs_ts.c, "API
 * thread"). The thread that owns the module — the browser Worker, or Node's
 * main thread — must never block: pthreads proxy their file-system calls to
 * it (including the QEMU thread's reads of the image through WORKERFS /
 * URLFS / NODEFS), and only it can start the Workers new kernel threads
 * need. So a call writes a request into linear memory, `anyfs_ts_api_submit`
 * queues it and returns at once, and the API thread reports completion
 * through `Module.anyfsApiDone(id)` on this thread.
 *
 * `ApiOp` and the request layout mirror `ANYFS_TS_OP_*` and
 * `struct anyfs_ts_req` in anyfs_ts.c — keep them in lockstep.
 */

/** Op numbers — mirror ANYFS_TS_OP_* in ts/native/anyfs_ts.c. */
export const ApiOp = {
    KERNEL_INIT: 1, // mem_mb, loglevel
    KERNEL_HALT: 2,
    SESSION_OPEN: 3, // path, flags
    SESSION_CLOSE: 4, // h
    SESSION_LIST: 5, // h, buf, cap
    SESSION_META: 6, // h, buf, cap
    SESSION_ENTER: 7, // h, part, flags, buf, cap
    READDIR: 8, // path, buf, cap
    LSTAT: 9, // path, buf, cap
    STAT: 10, // path, buf, cap
    REALPATH: 11, // path, buf, cap
    READLINK: 12, // path, buf, cap
    READ_KERNEL_FILE: 13, // path, buf, cap
    OPEN: 14, // path, flags
    PREAD: 15, // fd, buf, n, off_lo, off_hi
    CLOSE: 16, // fd
    LAST_ERROR: 17, // buf, cap
} as const;

/** struct anyfs_ts_req: int32 op, id, ret, arg[6]; pointer next. */
const REQ_WORDS = 10;
const REQ_BYTES = REQ_WORDS * 4;
const MAX_ARGS = 6;

/** Surface of the emscripten module the API calls need. */
export interface WasmApiModule {
    HEAPU8: Uint8Array;
    HEAP32: Int32Array;
    _malloc(n: number): number;
    _free(p: number): void;
    ccall(
        name: string,
        ret: 'number' | 'string' | null,
        argTypes: ReadonlyArray<'number' | 'string' | 'bigint'>,
        args: ReadonlyArray<number | string | bigint>,
    ): unknown;
    UTF8ToString(ptr: number, maxBytes?: number): string;
    stringToUTF8(s: string, ptr: number, maxBytes: number): void;
    lengthBytesUTF8(s: string): number;
    anyfsApiDone?: (id: number) => void;
}

const apis = new WeakMap<WasmApiModule, WasmApi>();

/** The WasmApi bound to `M` — one per module, since it owns the module's
 *  `anyfsApiDone` completion hook. */
export function wasmApiFor(M: WasmApiModule): WasmApi {
    let api = apis.get(M);
    if (!api) {
        api = new WasmApi(M);
        apis.set(M, api);
    }
    return api;
}

export class WasmApi {
    private readonly M: WasmApiModule;
    private readonly pending = new Map<
        number,
        { resolve: () => void; reject: (e: Error) => void }
    >();
    private readonly failCbs = new Set<(e: Error) => void>();
    private failure: Error | null = null;
    private nextId = 1;

    constructor(M: WasmApiModule) {
        this.M = M;
        M.anyfsApiDone = (id: number) => {
            const p = this.pending.get(id);
            this.pending.delete(id);
            p?.resolve();
        };
    }

    /** The error the module died with, or null while it is alive. */
    get failed(): Error | null {
        return this.failure;
    }

    /** Mark the module dead (it aborted): every pending and future call
     *  rejects with `err`, and onFail listeners run once. */
    fail(err: Error): void {
        if (this.failure) return;
        this.failure = err;
        for (const p of this.pending.values()) p.reject(err);
        this.pending.clear();
        for (const cb of this.failCbs) {
            try {
                cb(err);
            } catch {
                /* a listener throwing must not block the others */
            }
        }
        this.failCbs.clear();
    }

    /** Run `cb` once when the module dies (at once if it already has).
     *  Returns an unsubscribe fn. */
    onFail(cb: (err: Error) => void): () => void {
        if (this.failure) {
            cb(this.failure);
            return () => {};
        }
        this.failCbs.add(cb);
        return () => this.failCbs.delete(cb);
    }

    /** Run `op` on the API thread and resolve with its int result. String
     *  args are copied into the heap for the duration of the call. */
    async call(op: number, args: ReadonlyArray<number | string> = []): Promise<number> {
        if (this.failure) throw this.failure;
        if (args.length > MAX_ARGS) throw new Error(`wasm op ${op}: too many args`);
        const M = this.M;
        const req = M._malloc(REQ_BYTES);
        const strings: number[] = [];
        try {
            const w = req >> 2;
            M.HEAP32.fill(0, w, w + REQ_WORDS);
            const id = this.nextId++;
            M.HEAP32[w] = op;
            M.HEAP32[w + 1] = id;
            args.forEach((a, i) => {
                let v = a;
                if (typeof a === 'string') {
                    v = this.allocString(a);
                    strings.push(v);
                }
                M.HEAP32[w + 3 + i] = (v as number) | 0;
            });
            const done = new Promise<void>((resolve, reject) =>
                this.pending.set(id, { resolve, reject }),
            );
            if (M.ccall('anyfs_ts_api_submit', 'number', ['number'], [req]) !== 0) {
                this.pending.delete(id);
                throw new Error('anyfs_ts_api_submit: API thread unavailable');
            }
            await done;
            return M.HEAP32[w + 2] ?? -1;
        } finally {
            for (const p of strings) this.free(p);
            this.free(req);
        }
    }

    /** For ops that write into a (buf, cap) pair as their last two args and
     *  return the byte count, or -needed when cap is too small: grow and
     *  retry, then decode the bytes as UTF-8. */
    async callStringOut(
        op: number,
        args: ReadonlyArray<number | string>,
        name: string,
        initialCap = 8192,
    ): Promise<string> {
        const M = this.M;
        let cap = initialCap;
        for (let i = 0; i < 6; i++) {
            const buf = M._malloc(cap);
            try {
                const n = await this.call(op, [...args, buf, cap]);
                if (n >= 0) return M.UTF8ToString(buf, n);
                const need = -n;
                if (need <= cap) throw new Error(`${name}: rc=${n}`);
                cap = Math.max(need + 256, cap * 2);
            } finally {
                this.free(buf);
            }
        }
        throw new Error(`${name}: keeps requesting more buffer`);
    }

    /** callStringOut + JSON.parse. */
    async callJsonOut(
        op: number,
        args: ReadonlyArray<number | string>,
        name: string,
    ): Promise<unknown> {
        return JSON.parse(await this.callStringOut(op, args, name));
    }

    /** The API thread's last error message (set by the op that just failed),
     *  or '' if there is none. */
    async lastError(): Promise<string> {
        const M = this.M;
        const cap = 512;
        const buf = M._malloc(cap);
        try {
            const n = await this.call(ApiOp.LAST_ERROR, [buf, cap]);
            return n > 0 ? M.UTF8ToString(buf, n) : '';
        } finally {
            this.free(buf);
        }
    }

    /** Free heap memory — unless the module is dead: its heap can't be
     *  trusted, and calling into an aborted module throws. */
    free(p: number): void {
        if (!this.failure) this.M._free(p);
    }

    private allocString(s: string): number {
        const M = this.M;
        const n = M.lengthBytesUTF8(s) + 1;
        const p = M._malloc(n);
        M.stringToUTF8(s, p, n);
        return p;
    }
}
