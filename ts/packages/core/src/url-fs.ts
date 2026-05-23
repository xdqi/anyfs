/**
 * URLFS — an emscripten FS backend that reads a remote file over HTTP, on
 * demand, via synchronous `XMLHttpRequest` with `Range:` headers. Mirrors the
 * shape of emscripten's built-in WORKERFS so we can swap it in for a `File`
 * Blob without touching anyfs's `anyfs_ts_disk_open_p` path.
 *
 * Why sync XHR? `stream_ops.read` is invoked from inside an already
 * asyncified pread chain (see the readBuf comment in worker.ts). Async I/O
 * here would either pile a second asyncify hop on top — doubling cost — or
 * trip the "import changed state" abort we already work around for memory
 * growth. Sync XHR is still permitted in `DedicatedWorkerGlobalScope`, which
 * is exactly where the wasm runs.
 *
 * Cache: LRU of 512 KiB chunks, capped at 32 → ≈ 16 MiB ceiling per disk.
 * The kernel's pagecache sits on top, so steady-state random reads land
 * almost entirely in LKL's RAM rather than re-fetching.
 */

const FILE_MODE = 33279; // S_IFREG | 0o777
const DIR_MODE = 16895; // S_IFDIR | 0o777
const CHUNK_SIZE = 512 * 1024;
const MAX_CACHED_CHUNKS = 32;

// Errno values match emscripten's wasi mapping (see anyfs.mjs ERRNO_CODES).
const ENOENT = 44;
const EPERM = 63;
const EINVAL = 28;
const ESPIPE = 29;

interface UrlFsBackend {
    url: string;
    size: number;
    cache: Map<number, Uint8Array>;
}

interface UrlFsMountOpts {
    url: string;
    name: string;
}

function probeUrl(url: string): { size: number } {
    const xhr = new XMLHttpRequest();
    xhr.open('HEAD', url, false);
    xhr.send();
    if (xhr.status < 200 || xhr.status >= 300) {
        throw new Error(`URLFS: HEAD ${url} → HTTP ${xhr.status}`);
    }
    const cl = xhr.getResponseHeader('Content-Length');
    if (!cl) {
        throw new Error('URLFS: server did not return Content-Length on HEAD');
    }
    const size = Number.parseInt(cl, 10);
    if (!Number.isFinite(size) || size <= 0) {
        throw new Error(`URLFS: invalid Content-Length: ${cl}`);
    }
    const ar = (xhr.getResponseHeader('Accept-Ranges') ?? '').toLowerCase();
    if (!ar.includes('bytes')) {
        // Verify with a probe Range request — some servers omit the header but
        // honor the request anyway (e.g. nginx with default config).
        const probe = new XMLHttpRequest();
        probe.open('GET', url, false);
        probe.responseType = 'arraybuffer';
        probe.setRequestHeader('Range', 'bytes=0-0');
        probe.send();
        if (probe.status !== 206) {
            throw new Error(
                `URLFS: server lacks Accept-Ranges: bytes (got status ${probe.status} for probe range)`,
            );
        }
    }
    return { size };
}

function fetchChunk(backend: UrlFsBackend, idx: number): Uint8Array {
    const start = idx * CHUNK_SIZE;
    const end = Math.min(start + CHUNK_SIZE, backend.size) - 1;
    const xhr = new XMLHttpRequest();
    xhr.open('GET', backend.url, false);
    xhr.responseType = 'arraybuffer';
    xhr.setRequestHeader('Range', `bytes=${start}-${end}`);
    xhr.send();
    if (xhr.status !== 206 && xhr.status !== 200) {
        throw new Error(`URLFS: range ${start}-${end} → HTTP ${xhr.status}`);
    }
    let buf = new Uint8Array(xhr.response as ArrayBuffer);
    // Defensive: some servers ignore Range and return 200 with the full body.
    if (xhr.status === 200 && buf.length > end - start + 1) {
        buf = buf.subarray(start, end + 1);
    }
    return buf;
}

function getChunk(backend: UrlFsBackend, idx: number): Uint8Array {
    const hit = backend.cache.get(idx);
    if (hit) {
        // Touch for LRU ordering. Map keys iterate in insertion order; delete
        // + re-set moves this key to the tail, which is the most-recent slot.
        backend.cache.delete(idx);
        backend.cache.set(idx, hit);
        return hit;
    }
    const buf = fetchChunk(backend, idx);
    backend.cache.set(idx, buf);
    if (backend.cache.size > MAX_CACHED_CHUNKS) {
        const oldest = backend.cache.keys().next().value;
        if (oldest !== undefined) backend.cache.delete(oldest);
    }
    return buf;
}

// `M` is the emscripten Module; we touch FS internals (createNode,
// ErrnoError, isFile) that the typed worker shim doesn't surface.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function createUrlFs(M: any) {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const FS: any = M.FS;

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const URLFS: any = {
        FILE_MODE,
        DIR_MODE,

        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        mount(mount: any) {
            const opts = mount.opts as UrlFsMountOpts;
            const { size } = probeUrl(opts.url);
            const backend: UrlFsBackend = {
                url: opts.url,
                size,
                cache: new Map(),
            };
            const root = FS.createNode(null, '/', DIR_MODE, 0);
            root.mode = DIR_MODE;
            root.node_ops = URLFS.node_ops;
            root.stream_ops = URLFS.stream_ops;
            root.atime = root.mtime = root.ctime = Date.now();
            root.size = 4096;
            root.contents = {};
            URLFS.createFileNode(root, opts.name, backend);
            return root;
        },

        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        createFileNode(parent: any, name: string, backend: UrlFsBackend) {
            const node = FS.createNode(parent, name, FILE_MODE);
            node.mode = FILE_MODE;
            node.node_ops = URLFS.node_ops;
            node.stream_ops = URLFS.stream_ops;
            node.atime = node.mtime = node.ctime = Date.now();
            node.size = backend.size;
            node.contents = backend;
            parent.contents[name] = node;
            return node;
        },

        node_ops: {
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            getattr(node: any) {
                return {
                    dev: 1,
                    ino: node.id,
                    mode: node.mode,
                    nlink: 1,
                    uid: 0,
                    gid: 0,
                    rdev: 0,
                    size: node.size,
                    atime: new Date(node.atime),
                    mtime: new Date(node.mtime),
                    ctime: new Date(node.ctime),
                    blksize: 4096,
                    blocks: Math.ceil(node.size / 4096),
                };
            },
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            setattr(node: any, attr: any) {
                for (const key of ['mode', 'atime', 'mtime', 'ctime'] as const) {
                    if (attr[key] != null) node[key] = attr[key];
                }
            },
            lookup() {
                throw new FS.ErrnoError(ENOENT);
            },
            mknod() {
                throw new FS.ErrnoError(EPERM);
            },
            rename() {
                throw new FS.ErrnoError(EPERM);
            },
            unlink() {
                throw new FS.ErrnoError(EPERM);
            },
            rmdir() {
                throw new FS.ErrnoError(EPERM);
            },
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            readdir(node: any) {
                const entries = ['.', '..'];
                for (const key of Object.keys(node.contents)) entries.push(key);
                return entries;
            },
            symlink() {
                throw new FS.ErrnoError(EPERM);
            },
        },

        stream_ops: {
            read(
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                stream: any,
                buffer: Uint8Array,
                offset: number,
                length: number,
                position: number,
            ) {
                const backend = stream.node.contents as UrlFsBackend;
                if (position >= backend.size) return 0;
                const end = Math.min(position + length, backend.size);
                let copied = 0;
                let pos = position;
                while (pos < end) {
                    const idx = Math.floor(pos / CHUNK_SIZE);
                    const chunkStart = idx * CHUNK_SIZE;
                    const chunk = getChunk(backend, idx);
                    const inChunkOff = pos - chunkStart;
                    const want = Math.min(end - pos, chunk.length - inChunkOff);
                    buffer.set(chunk.subarray(inChunkOff, inChunkOff + want), offset + copied);
                    copied += want;
                    pos += want;
                }
                return copied;
            },
            write() {
                throw new FS.ErrnoError(ESPIPE);
            },
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            llseek(stream: any, offset: number, whence: number) {
                let position = offset;
                if (whence === 1) position += stream.position;
                else if (whence === 2 && FS.isFile(stream.node.mode)) {
                    position += stream.node.size;
                }
                if (position < 0) throw new FS.ErrnoError(EINVAL);
                return position;
            },
        },
    };

    return URLFS;
}
