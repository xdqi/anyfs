/** The known tree every populated base image holds. */
import { lutimesSync, mkdirSync, rmSync, symlinkSync, utimesSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { rng } from './rng.mjs';

/** 2026-01-01T00:00:00Z: every file's mtime, and the fake mkfs clock. */
export const EPOCH = 1767225600;

/** `docs/` is the directory whose inode the ext4 zero-inode mutations hit. */
export const TREE = [
    { path: 'hello.txt', text: 'hello, anyfs\n' },
    { path: 'docs/readme.md', text: '# anyfs robustness corpus\n'.repeat(80) },
    { path: 'docs/nested/deep/leaf.txt', text: 'leaf\n' },
    { path: 'data/blob.bin', random: 300 * 1024, seed: 1 },
    { path: 'data/small.bin', random: 4096, seed: 2 },
    { path: 'link', symlink: 'hello.txt' },
];

function randomBytes(n, seed) {
    const next = rng(seed);
    const b = Buffer.alloc(n);
    for (let i = 0; i < n; i++) b[i] = Math.floor(next() * 256);
    return b;
}

/** Write TREE under `dir` (wiped first). `symlinks: false` drops the
 *  symlink, for filesystems that cannot store one (FAT). */
export function writeTree(dir, { symlinks = true } = {}) {
    rmSync(dir, { recursive: true, force: true });
    mkdirSync(dir, { recursive: true });
    const paths = new Set();
    for (const e of TREE) {
        if (e.symlink && !symlinks) continue;
        const p = join(dir, e.path);
        mkdirSync(dirname(p), { recursive: true });
        if (e.symlink) symlinkSync(e.symlink, p);
        else if (e.text) writeFileSync(p, e.text);
        else writeFileSync(p, randomBytes(e.random, e.seed));
        for (let q = e.path; q !== '.'; q = dirname(q)) paths.add(q);
    }
    for (const q of paths) lutimesSync(join(dir, q), EPOCH, EPOCH);
    utimesSync(dir, EPOCH, EPOCH);
}
