// Minimal Node smoke test for the wasm bundle.
// Exercises: createAnyfsModule() -> NODEFS mount -> kernel boot ->
// session open / list / enter / readdir (+ pread on big) against disk images.
//
// Talks to the bundle's ABI directly, without the built @anyfs/core: every op
// goes through the API thread (anyfs_ts_api_submit + Module.anyfsApiDone),
// the same protocol src/wasm-api.ts implements. This thread owns the module
// and must never block — the QEMU thread proxies its NODEFS reads to it.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const { default: createAnyfsModule } = await import(
    new URL('../wasm/anyfs.node.mjs', import.meta.url).href
);

const DISKS_DIR = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    '../../../examples/vite-demo/public/disks',
);

const IMAGES = {
    single: path.join(DISKS_DIR, 'single.img'),
    multi: path.join(DISKS_DIR, 'multi.img'),
    big: path.join(DISKS_DIR, 'big.img'),
};

const which = process.argv[2] || 'multi';
const imgLink = IMAGES[which];
if (!imgLink) {
    console.error('unknown image:', which, 'choose: single|multi|big');
    process.exit(2);
}
// Some disk images are symlinks pointing outside DISKS_DIR. NODEFS exposes
// the symlink as-is, and Emscripten's VFS resolves its target inside the
// wasm namespace (where it doesn't exist) — so mount the realpath'd
// directory and open the real file name instead.
const imgHost = fs.realpathSync(imgLink);

console.log('[smoke] loading wasm module…');
const M = await createAnyfsModule({
    preRun: [
        (m) => {
            m.FS.mkdir('/work');
            m.FS.mount(m.NODEFS, { root: path.dirname(imgHost) }, '/work');
        },
    ],
});
console.log('[smoke] module loaded; main() ran automatically');

// API-thread call: mirrors ANYFS_TS_OP_* / struct anyfs_ts_req in
// ts/native/anyfs_ts.c (int32 op, id, ret, arg[6]; pointer next).
const OP = {
    KERNEL_INIT: 1,
    KERNEL_HALT: 2,
    SESSION_OPEN: 3,
    SESSION_CLOSE: 4,
    SESSION_LIST: 5,
    SESSION_ENTER: 7,
    READDIR: 8,
    OPEN: 14,
    PREAD: 15,
    CLOSE: 16,
    LAST_ERROR: 17,
};
const pending = new Map();
let nextId = 1;
M.anyfsApiDone = (id) => {
    pending.get(id)?.();
    pending.delete(id);
};
async function api(op, args = []) {
    const req = M._malloc(40);
    const strs = [];
    try {
        const w = req >> 2;
        M.HEAP32.fill(0, w, w + 10);
        const id = nextId++;
        M.HEAP32[w] = op;
        M.HEAP32[w + 1] = id;
        args.forEach((a, i) => {
            let v = a;
            if (typeof a === 'string') {
                const n = M.lengthBytesUTF8(a) + 1;
                v = M._malloc(n);
                M.stringToUTF8(a, v, n);
                strs.push(v);
            }
            M.HEAP32[w + 3 + i] = v | 0;
        });
        const done = new Promise((resolve) => pending.set(id, resolve));
        if (M.ccall('anyfs_ts_api_submit', 'number', ['number'], [req]) !== 0) {
            throw new Error('anyfs_ts_api_submit failed');
        }
        await done;
        return M.HEAP32[w + 2];
    } finally {
        strs.forEach((p) => M._free(p));
        M._free(req);
    }
}
async function lastError() {
    const buf = M._malloc(512);
    try {
        const n = await api(OP.LAST_ERROR, [buf, 512]);
        return n > 0 ? M.UTF8ToString(buf, n) : '';
    } finally {
        M._free(buf);
    }
}

console.log('[smoke] kernel init(64, 0)…');
{
    const rc = await api(OP.KERNEL_INIT, [64, 0]);
    console.log('  rc =', rc);
    if (rc !== 0) process.exit(3);
}

const fsPath = '/work/' + path.basename(imgHost);
console.log('[smoke] session open(', fsPath, ', 0)…');
const h = await api(OP.SESSION_OPEN, [fsPath, 0]);
console.log('  handle =', h);
if (h < 0) {
    console.error('  error:', await lastError());
    process.exit(4);
}

const cap = 4096;
const bufPtr = M._malloc(cap);
const n = await api(OP.SESSION_LIST, [h, bufPtr, cap]);
console.log('  list rc =', n);
if (n < 0) {
    console.error('list_json overflow, needs', -n, 'bytes');
    process.exit(5);
}
const json = M.UTF8ToString(bufPtr, n);
console.log('  partitions =', json);
M._free(bufPtr);

// Now exercise enter()/readdir()/pread() against an ext4 image.
const exerciseEntry =
    which === 'big'
        ? { mountWhole: 'ext4' }
        : which === 'single'
          ? { mountWhole: 'ext4' }
          : { part: 3 }; // multi-part: ext2 partition (no journal replay needed)

let mountPath;
const mountBuf = M._malloc(128);
const enterPart = exerciseEntry.mountWhole ? 0 : exerciseEntry.part;
const rc2 = await api(OP.SESSION_ENTER, [h, enterPart, 0, mountBuf, 128]);
console.log('[smoke] session_enter rc =', rc2);
if (rc2 !== 0) {
    console.error('  error:', await lastError());
    process.exit(6);
}
mountPath = M.UTF8ToString(mountBuf);
M._free(mountBuf);
console.log('  mounted at', mountPath);

const ddBuf = M._malloc(8192);
const ddRc = await api(OP.READDIR, [mountPath, ddBuf, 8192]);
console.log('[smoke] readdir rc =', ddRc);
if (ddRc < 0) {
    console.error('readdir overflow, needs', -ddRc);
    process.exit(7);
}
const ddJson = M.UTF8ToString(ddBuf, ddRc);
console.log('  entries =', ddJson);
M._free(ddBuf);

// If big_ext4, also pread the first 16 bytes of big.bin.
if (which === 'big') {
    const fdPath = mountPath + '/big.bin';
    const fd = await api(OP.OPEN, [fdPath, 0]);
    console.log('[smoke] open(big.bin) fd =', fd);
    if (fd < 0) process.exit(8);
    const rbuf = M._malloc(16);
    const got = await api(OP.PREAD, [fd, rbuf, 16, 0, 0]);
    console.log('[smoke] pread(16,0) ret =', got);
    const bytes = new Uint8Array(M.HEAPU8.buffer, rbuf, 16).slice();
    console.log('  bytes =', Buffer.from(bytes).toString('hex'));
    M._free(rbuf);
    await api(OP.CLOSE, [fd]);
}

await api(OP.SESSION_CLOSE, [h]);
await api(OP.KERNEL_HALT);
console.log('[smoke] OK');
process.exit(0);
