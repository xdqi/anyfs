// Minimal Node smoke test for the wasm bundle.
// Exercises: createAnyfsModule() -> NODEFS mount -> anyfs_ts_init ->
// anyfs_ts_disk_open + anyfs_ts_disk_list_json against disk_multi.img.
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const require = createRequire(import.meta.url);

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
const imgHost = IMAGES[which];
if (!imgHost) {
    console.error('unknown image:', which, 'choose: single|multi|big');
    process.exit(2);
}

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

console.log('[smoke] anyfs_ts_init(64, 0)…');
const rc = M.ccall('anyfs_ts_init', 'number', ['number', 'number'], [64, 0]);
console.log('  rc =', rc);
if (rc !== 0) process.exit(3);

const fsPath = '/work/' + path.basename(imgHost);
console.log('[smoke] anyfs_ts_disk_open(', fsPath, ', 0)…');
const h = M.ccall('anyfs_ts_disk_open', 'number', ['string', 'number'], [fsPath, 0]);
console.log('  handle =', h);
if (h < 0) process.exit(4);

const cap = 4096;
const bufPtr = M._malloc(cap);
const n = M.ccall(
    'anyfs_ts_disk_list_json',
    'number',
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
if (exerciseEntry.mountWhole) {
    const rc2 = M.ccall(
        'anyfs_ts_mount_whole',
        'number',
        ['number', 'string', 'number', 'number', 'number'],
        [h, exerciseEntry.mountWhole, 0, mountBuf, 128],
    );
    console.log('[smoke] mount_whole rc =', rc2);
    if (rc2 !== 0) process.exit(6);
    mountPath = M.UTF8ToString(mountBuf);
} else {
    const rc2 = M.ccall(
        'anyfs_ts_disk_enter',
        'number',
        ['number', 'number', 'number', 'number', 'number'],
        [h, exerciseEntry.part, 0, mountBuf, 128],
    );
    console.log('[smoke] disk_enter rc =', rc2);
    if (rc2 !== 0) process.exit(6);
    mountPath = M.UTF8ToString(mountBuf);
}
M._free(mountBuf);
console.log('  mounted at', mountPath);

const ddBuf = M._malloc(8192);
const ddRc = M.ccall(
    'anyfs_ts_readdir_json',
    'number',
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
    const fd = M.ccall('anyfs_ts_open', 'number', ['string', 'number'], [fdPath, 0]);
    console.log('[smoke] open(big.bin) fd =', fd);
    if (fd < 0) process.exit(8);
    const rbuf = M._malloc(16);
    const got = M.ccall(
        'anyfs_ts_pread',
        'number',
        ['number', 'number', 'number', 'bigint'],
        [fd, rbuf, 16, 0n],
    );
    console.log('[smoke] pread(16,0) ret =', got);
    const bytes = new Uint8Array(M.HEAPU8.buffer, rbuf, 16).slice();
    console.log('  bytes =', Buffer.from(bytes).toString('hex'));
    M._free(rbuf);
    M.ccall('anyfs_ts_close', 'number', ['number'], [fd]);
}

M.ccall('anyfs_ts_disk_close', 'number', ['number'], [h]);
M.ccall('anyfs_ts_kernel_halt', 'number', [], []);
console.log('[smoke] OK');
process.exit(0);
