import type { AnyfsModule } from './module.js';
import type { DirEntry, DiskHandle, EnterOpts, LklFd, PartInfo, Stat } from './types.js';

/** Default initial buffer for JSON syscalls (bytes). Grows on demand. */
const JSON_BUF_INIT = 4096;

/** Read a NEGATIVE-on-overflow JSON helper into a string. Retries with the
 * buffer size the C side asks for. */
function callJsonStringArg(M: AnyfsModule, fnName: string, pathArg: string): string {
    let cap = JSON_BUF_INIT;
    while (true) {
        const buf = M._malloc(cap);
        try {
            const ret = M.ccall(
                fnName,
                'number',
                ['string', 'number', 'number'],
                [pathArg, buf, cap],
            ) as number;
            if (ret < 0) {
                cap = Math.max(cap * 2, -ret);
                continue;
            }
            return M.UTF8ToString(buf, ret);
        } finally {
            M._free(buf);
        }
    }
}

function callJsonHandleArg(M: AnyfsModule, fnName: string, h: DiskHandle): string {
    let cap = JSON_BUF_INIT;
    while (true) {
        const buf = M._malloc(cap);
        try {
            const ret = M.ccall(
                fnName,
                'number',
                ['number', 'number', 'number'],
                [h, buf, cap],
            ) as number;
            if (ret < 0) {
                cap = Math.max(cap * 2, -ret);
                continue;
            }
            return M.UTF8ToString(buf, ret);
        } finally {
            M._free(buf);
        }
    }
}

export class AnyfsDisk {
    private readonly M: AnyfsModule;
    private h: DiskHandle;
    private fds = new Set<LklFd>();
    private disposed = false;

    /** @internal — use mountFile() in core/index.ts. */
    constructor(M: AnyfsModule, handle: DiskHandle) {
        this.M = M;
        this.h = handle;
    }

    private check(): void {
        if (this.disposed) throw new Error('AnyfsDisk: already disposed');
    }

    async listPartitions(): Promise<PartInfo[]> {
        this.check();
        const json = callJsonHandleArg(this.M, 'anyfs_ts_disk_list_json', this.h);
        return JSON.parse(json) as PartInfo[];
    }

    /** Mount partition by slot_id; returns the LKL mount path. */
    async enter(part: number, opts: EnterOpts = {}): Promise<string> {
        this.check();
        const flags = opts.flags ?? 0;
        const cap = 128;
        const buf = this.M._malloc(cap);
        try {
            const rc = this.M.ccall(
                'anyfs_ts_disk_enter',
                'number',
                ['number', 'number', 'number', 'number', 'number'],
                [this.h, part, flags, buf, cap],
            ) as number;
            if (rc !== 0) throw new Error(`disk_enter failed: rc=${rc}`);
            return this.M.UTF8ToString(buf);
        } finally {
            this.M._free(buf);
        }
    }

    /** Mount the whole disk (no partition table) under the given fstype. */
    async mountWhole(fstype?: string): Promise<string> {
        this.check();
        const cap = 128;
        const buf = this.M._malloc(cap);
        try {
            const rc = this.M.ccall(
                'anyfs_ts_mount_whole',
                'number',
                ['number', 'string', 'number', 'number', 'number'],
                [this.h, fstype ?? '', 0, buf, cap],
            ) as number;
            if (rc !== 0) throw new Error(`mount_whole failed: rc=${rc}`);
            return this.M.UTF8ToString(buf);
        } finally {
            this.M._free(buf);
        }
    }

    async readdir(path: string): Promise<DirEntry[]> {
        this.check();
        const json = callJsonStringArg(this.M, 'anyfs_ts_readdir_json', path);
        return JSON.parse(json) as DirEntry[];
    }

    async stat(path: string): Promise<Stat> {
        this.check();
        const json = callJsonStringArg(this.M, 'anyfs_ts_lstat_json', path);
        return JSON.parse(json) as Stat;
    }

    async open(path: string, flags = 0): Promise<LklFd> {
        this.check();
        const fd = this.M.ccall(
            'anyfs_ts_open',
            'number',
            ['string', 'number'],
            [path, flags],
        ) as number;
        if (fd < 0) throw new Error(`open(${path}) failed: ${fd}`);
        this.fds.add(fd);
        return fd;
    }

    async read(fd: LklFd, offset: number, length: number): Promise<Uint8Array> {
        this.check();
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

    async close(fd: LklFd): Promise<void> {
        this.check();
        const rc = this.M.ccall('anyfs_ts_close', 'number', ['number'], [fd]) as number;
        this.fds.delete(fd);
        if (rc < 0) throw new Error(`close(${fd}) failed: ${rc}`);
    }

    /** See WorkerAnyfsDisk.openReadable for the contract. */
    async openReadable(
        path: string,
        opts: { chunkSize?: number } = {},
    ): Promise<{ stream: ReadableStream<Uint8Array>; size: number }> {
        const chunkSize = opts.chunkSize ?? 1024 * 1024;
        const st = await this.stat(path);
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

    async dispose(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        for (const fd of this.fds) {
            try {
                this.M.ccall('anyfs_ts_close', 'number', ['number'], [fd]);
            } catch {
                /* best effort */
            }
        }
        this.fds.clear();
        this.M.ccall('anyfs_ts_disk_close', 'number', ['number'], [this.h]);
    }
}
