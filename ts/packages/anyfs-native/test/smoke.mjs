import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';

const require = createRequire(import.meta.url);
const here = dirname(fileURLToPath(import.meta.url));
const n = require('../index.js');
const img = resolve(here, '../../../examples/vite-demo/public/disks/multi.img');

console.log('[smoke] kernelInit(64, 0)');
if (n.kernelInit(64, 0) !== 0) process.exit(3);

console.log('[smoke] sessionOpen', img);
const h = n.sessionOpen(img, 0);
if (h < 0) {
    console.error('open rc=', h);
    process.exit(4);
}

const parts = JSON.parse(n.sessionListJson(h));
console.log(
    '[smoke] partitions:',
    parts.length,
    parts.map((p) => `${p.fstype}/${p.label}`).join(', '),
);
if (parts.length === 0) process.exit(5);

const meta = JSON.parse(n.sessionMetaJson(h));
console.log('[smoke] sessionMeta:', meta);

// Pick the first non-journaled FS so RDONLY mount doesn't need replay.
const pick = parts.find((p) => p.fstype === 'ext2') ?? parts[0];
console.log(`[smoke] sessionEnter(part=${pick.index} ${pick.fstype}/${pick.label}, RDONLY)`);
const mount = n.sessionEnter(h, pick.index, 1); // ANYFS_MOUNT_RDONLY
console.log('  mounted at', mount);

const entries = JSON.parse(n.readdirJson(mount));
console.log('[smoke] readdir:', entries.length, 'entries');
console.log(' ', entries.slice(0, 5));

const meta2 = JSON.parse(n.lstatJson(mount));
console.log('[smoke] lstat(mount):', { kind: meta2.kind, mode: meta2.mode.toString(8) });

const firstFile = entries.find((e) => e.kind === 'file');
if (firstFile) {
    const fpath = `${mount}/${firstFile.name}`;
    const st = JSON.parse(n.statJson(fpath));
    console.log('[smoke] stat', firstFile.name, 'size=', st.size);
    const fd = n.fileOpen(fpath, 0);
    if (fd < 0) {
        console.error('open rc=', fd);
        process.exit(6);
    }
    const buf = Buffer.alloc(Math.min(64, st.size));
    const got = n.pread(fd, buf, buf.length, 0);
    console.log(
        '[smoke] pread got=',
        got,
        'first bytes:',
        buf.subarray(0, Math.min(16, got)).toString('hex'),
    );
    n.fileClose(fd);
} else {
    console.log('[smoke] no regular files in mount root (skipping pread)');
}

if (n.sessionClose(h) !== 0) console.warn('sessionClose nonzero');
console.log('[smoke] OK');
