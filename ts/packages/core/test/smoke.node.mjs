// Minimal Node smoke test for the wasm bundle.
// Exercises: createAnyfsModule() -> NODEFS mount -> kernel boot ->
// anyfs_ts_session_open_p + anyfs_ts_session_list_json_p against disk images.
//
// Call pattern mirrors src/worker.ts: every entry point that can touch the
// QEMU block layer goes through the `_p` out-pointer variant with
// ccall({async: true}). The block layer runs on emscripten fibers; a fiber
// swap (or a kernel-side wait that defers to the event loop) leaves the
// synchronous ccall path mid-unwind ("running asynchronously" assertion)
// and discards the export's return value. Sync ccall is only safe for
// calls that never reach the block layer (init/poll/halt).
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

// Async ccall against a `_p` out-pointer variant (same shape as worker.ts
// callP): the wasm export's direct return value is discarded by the fiber
// rewind path, so the C side writes the result through the trailing
// int32_t* out parameter.
const outp = M._malloc(4);
async function callP(name, argTypes, args) {
    M.HEAP32[outp >> 2] = -0x7fffffff;
    await M.ccall(name, null, [...argTypes, 'number'], [...args, outp], { async: true });
    return M.HEAP32[outp >> 2];
}
async function callA(name, retType, argTypes, args) {
    return await M.ccall(name, retType, argTypes, args, { async: true });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Boot: prefer the dedicated-pthread async path when the bundle exports it
// (keeps this thread's event loop free to service kthread spawns).
if (M._anyfs_ts_init_async) {
    console.log('[smoke] anyfs_ts_init_async(64, 0)…');
    const arc = M.ccall('anyfs_ts_init_async', 'number', ['number', 'number'], [64, 0]);
    if (arc !== 0) process.exit(3);
    for (let i = 0; i < 600; i++) {
        if (M.ccall('anyfs_ts_is_boot_complete', 'number', [], [])) break;
        await sleep(100);
    }
    const rc = M.ccall('anyfs_ts_boot_result', 'number', [], []);
    console.log('  rc =', rc);
    if (rc !== 0) process.exit(3);
} else {
    console.log('[smoke] anyfs_ts_kernel_init(64, 0)…');
    const rc = await callA('anyfs_ts_kernel_init', 'number', ['number', 'number'], [64, 0]);
    console.log('  rc =', rc);
    if (rc !== 0) process.exit(3);
}

const fsPath = '/work/' + path.basename(imgHost);
console.log('[smoke] anyfs_ts_session_open_p(', fsPath, ', 0)…');
const h = await callP('anyfs_ts_session_open_p', ['string', 'number'], [fsPath, 0]);
console.log('  handle =', h);
if (h < 0) process.exit(4);

const cap = 4096;
const bufPtr = M._malloc(cap);
const n = await callP(
    'anyfs_ts_session_list_json_p',
    ['number', 'number', 'number'],
    [h, bufPtr, cap],
);
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
// Prefer the dedicated-pthread enter (ext4's jbd2 kthread can't spawn while
// the entering thread is blocked); fall back to the _p variant.
let rc2;
if (M._anyfs_ts_session_enter_async) {
    rc2 = M.ccall(
        'anyfs_ts_session_enter_async',
        'number',
        ['number', 'number', 'number'],
        [h, enterPart, 0],
    );
    if (rc2 === 0) {
        let done = 0;
        for (let i = 0; i < 600; i++) {
            done = M.ccall('anyfs_ts_session_enter_is_complete', 'number', [], []);
            if (done) break;
            await sleep(100);
        }
        if (!done) {
            console.error('session_enter timed out');
            process.exit(6);
        }
        rc2 = await callP(
            'anyfs_ts_session_enter_result_p',
            ['number', 'number'],
            [mountBuf, 128],
        );
    }
} else {
    rc2 = await callP(
        'anyfs_ts_session_enter_p',
        ['number', 'number', 'number', 'number', 'number'],
        [h, enterPart, 0, mountBuf, 128],
    );
}
console.log('[smoke] session_enter rc =', rc2);
if (rc2 !== 0) process.exit(6);
mountPath = M.UTF8ToString(mountBuf);
M._free(mountBuf);
console.log('  mounted at', mountPath);

const ddBuf = M._malloc(8192);
const ddRc = await callP(
    'anyfs_ts_readdir_json_p',
    ['string', 'number', 'number'],
    [mountPath, ddBuf, 8192],
);
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
    const fd = await callP('anyfs_ts_open_p', ['string', 'number'], [fdPath, 0]);
    console.log('[smoke] open(big.bin) fd =', fd);
    if (fd < 0) process.exit(8);
    const rbuf = M._malloc(16);
    const got = await callP(
        'anyfs_ts_pread_p',
        ['number', 'number', 'number', 'number', 'number'],
        [fd, rbuf, 16, 0, 0],
    );
    console.log('[smoke] pread(16,0) ret =', got);
    const bytes = new Uint8Array(M.HEAPU8.buffer, rbuf, 16).slice();
    console.log('  bytes =', Buffer.from(bytes).toString('hex'));
    M._free(rbuf);
    await callP('anyfs_ts_close_p', ['number'], [fd]);
}

await callA('anyfs_ts_session_close', 'number', ['number'], [h]);
await callA('anyfs_ts_kernel_halt', 'number', [], []);
M._free(outp);
console.log('[smoke] OK');
process.exit(0);
