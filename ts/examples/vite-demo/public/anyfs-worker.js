// Hosts the entire anyfs wasm module inside a Web Worker so:
//  - WORKERFS.mount(...) passes its assert(ENVIRONMENT_IS_WORKER)
//  - LKL's blocking syscalls (sem_wait → Atomics.wait) work, since
//    Chrome only allows Atomics.wait inside workers, not the main JS thread.
//
// Protocol: postMessage({id, op, args}) → postMessage({id, ok, result|error}).
// op set: mount, listPartitions, mountWhole, enter, readdir, stat, open, read, close, dispose.

let M = null;
let diskHandle = -1;

const send = (msg) => self.postMessage(msg);

self.addEventListener('error', (e) => {
    send({ event: 'host-error', message: e.message, stack: e.error?.stack, filename: e.filename, lineno: e.lineno });
});
self.addEventListener('unhandledrejection', (e) => {
    send({ event: 'host-rejection', message: e.reason?.message ?? String(e.reason), stack: e.reason?.stack });
});

const ops = {
    async mount({ file, memMb = 64, loglevel = 0, wasmDir = '/wasm/' }) {
        send({ event: 'progress', step: 'importing wasm shim' });
        const mod = await import(`${wasmDir}anyfs.mjs`);
        send({ event: 'progress', step: 'shim imported' });

        const factory = mod.default;
        const fsPath = `/work/${file.name || 'image'}`;
        M = await factory({
            print: (m) => send({ event: 'stdout', message: m }),
            printErr: (m) => send({ event: 'stderr', message: m }),
            locateFile: (p) => new URL(`${wasmDir}${p}`, self.location.href).href,
            onAbort: (reason) => send({ event: 'abort', reason: String(reason) }),
            preRun: [(m) => {
                if (!m.WORKERFS) throw new Error('WORKERFS missing');
                m.FS.mkdir('/work');
                m.FS.mount(m.WORKERFS, {
                    blobs: [{ name: file.name || 'image', data: file }],
                }, '/work');
                send({ event: 'progress', step: 'WORKERFS mounted' });
            }],
        });
        send({ event: 'progress', step: 'factory resolved' });

        const rc = M.ccall('anyfs_ts_init', 'number', ['number', 'number'], [memMb, loglevel]);
        send({ event: 'progress', step: `anyfs_ts_init -> ${rc}` });
        if (rc !== 0) throw new Error(`anyfs_ts_init failed: ${rc}`);

        // flags: ANYFS_DISK_READONLY = 1 (WORKERFS-backed image is read-only)
        diskHandle = M.ccall('anyfs_ts_disk_open', 'number', ['string', 'number'], [fsPath, 1]);
        send({ event: 'progress', step: `anyfs_ts_disk_open -> ${diskHandle}` });
        if (diskHandle < 0) throw new Error(`anyfs_ts_disk_open failed: ${diskHandle}`);

        return { diskHandle };
    },

    listPartitions() {
        if (!M || diskHandle < 0) throw new Error('not mounted');
        return callJsonOut('anyfs_ts_disk_list_json', ['number'], [diskHandle]);
    },

    mountWhole({ fstype }) {
        if (!M || diskHandle < 0) throw new Error('not mounted');
        const cap = 128;
        const out = M._malloc(cap);
        try {
            // ANYFS_MOUNT_RDONLY = 1 (WORKERFS image can't be written; ext4 needs noload to skip journal)
            const rc = M.ccall('anyfs_ts_mount_whole', 'number',
                ['number', 'string', 'number', 'number', 'number'],
                [diskHandle, fstype || '', 1, out, cap]);
            if (rc < 0) throw new Error(`mount_whole rc=${rc}`);
            return M.UTF8ToString(out);
        } finally { M._free(out); }
    },

    enter({ part }) {
        if (!M || diskHandle < 0) throw new Error('not mounted');
        const cap = 128;
        const out = M._malloc(cap);
        try {
            const rc = M.ccall('anyfs_ts_disk_enter', 'number',
                ['number', 'number', 'number', 'number', 'number'],
                [diskHandle, part, 1, out, cap]);
            if (rc < 0) throw new Error(`disk_enter rc=${rc}`);
            return M.UTF8ToString(out);
        } finally { M._free(out); }
    },

    readdir({ path }) {
        if (!M) throw new Error('not mounted');
        return callJsonOut('anyfs_ts_readdir_json', ['string'], [path]);
    },

    stat({ path }) {
        if (!M) throw new Error('not mounted');
        return callJsonOut('anyfs_ts_lstat_json', ['string'], [path]);
    },

    open({ path }) {
        if (!M) throw new Error('not mounted');
        return M.ccall('anyfs_ts_open', 'number', ['string'], [path]);
    },

    read({ fd, offset, length }) {
        if (!M) throw new Error('not mounted');
        const buf = M._malloc(length);
        try {
            const n = Number(M.ccall('anyfs_ts_pread', 'bigint',
                ['number', 'number', 'number', 'bigint'],
                [fd, buf, length, BigInt(offset)]));
            if (n < 0) throw new Error(`pread rc=${n}`);
            return new Uint8Array(M.HEAPU8.subarray(buf, buf + n).slice());
        } finally { M._free(buf); }
    },

    close({ fd }) {
        if (!M) throw new Error('not mounted');
        return M.ccall('anyfs_ts_close', 'number', ['number'], [fd]);
    },

    dispose() {
        if (!M) return 0;
        if (diskHandle >= 0) {
            try { M.ccall('anyfs_ts_disk_close', 'number', ['number'], [diskHandle]); } catch {}
            diskHandle = -1;
        }
        try { M.ccall('anyfs_ts_kernel_halt', 'number', [], []); } catch {}
        M = null;
        return 0;
    },
};

// Variable-output JSON helper with growing buffer.
function callJsonOut(name, argTypes, args) {
    let cap = 8192;
    for (let i = 0; i < 6; i++) {
        const buf = M._malloc(cap);
        try {
            const n = M.ccall(name, 'number', [...argTypes, 'number', 'number'], [...args, buf, cap]);
            if (n >= 0) {
                const s = M.UTF8ToString(buf, n);
                return JSON.parse(s);
            }
            // n < 0 → need at least -n bytes
            const need = -n;
            if (need <= cap) throw new Error(`${name} negative rc=${n}`);
            cap = Math.max(need + 256, cap * 2);
        } finally { M._free(buf); }
    }
    throw new Error(`${name} keeps requesting more buffer`);
}

self.addEventListener('message', async (e) => {
    const { id, op, args } = e.data;
    try {
        const fn = ops[op];
        if (!fn) throw new Error(`unknown op: ${op}`);
        const result = await fn(args || {});
        send({ id, ok: true, result });
    } catch (err) {
        send({ id, ok: false, error: err?.message ?? String(err), stack: err?.stack });
    }
});

send({ event: 'host-ready' });
