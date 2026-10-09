#!/usr/bin/env node
/*
 * Copy files out of a disk image with anyfs itself (the Linux native addon),
 * read-only: whole-disk filesystem, or a partition with --part N.
 *
 *   extract_from_image.mjs --addon PATH [--part N] IMAGE OUTDIR FILE...
 *
 * Each FILE (a path inside the filesystem) lands in OUTDIR under its base
 * name. build_macos_sysroot.sh uses it to take macFUSE's installer package
 * out of the macFUSE .dmg (an HFS+ volume) on Linux, where no other tool
 * reads HFS+.
 */
import { closeSync, mkdirSync, openSync, writeSync } from 'node:fs';
import { createRequire } from 'node:module';
import { basename, join, resolve } from 'node:path';

const require = createRequire(import.meta.url);
const args = process.argv.slice(2);
let addon = null;
let part = 0;
const rest = [];
for (let i = 0; i < args.length; i++) {
    if (args[i] === '--addon') addon = args[++i];
    else if (args[i] === '--part') part = Number(args[++i]);
    else rest.push(args[i]);
}
const [image, outdir, ...files] = rest;
if (!addon || !image || !outdir || files.length === 0) {
    console.error('usage: extract_from_image.mjs --addon PATH [--part N] IMAGE OUTDIR FILE...');
    process.exit(2);
}

const n = require(resolve(addon));
let rc = 0;
try {
    if ((await n.kernelInit(256, 0)) !== 0) throw new Error('kernelInit failed');
    const h = await n.sessionOpen(resolve(image), 1 /* ANYFS_SESSION_READONLY */);
    const mnt = await n.sessionEnter(h, part, 1 /* ANYFS_MOUNT_RDONLY */);
    mkdirSync(outdir, { recursive: true });
    for (const f of files) {
        const fd = await n.fileOpen(`${mnt}/${f}`, 0);
        if (fd < 0) throw new Error(`${f}: open failed (${fd})`);
        const out = openSync(join(outdir, basename(f)), 'w');
        let off = 0;
        for (;;) {
            const { rc: got, data } = await n.pread(fd, 1 << 20, off);
            if (got < 0) throw new Error(`${f}: read failed at ${off} (${got})`);
            if (got === 0) break;
            writeSync(out, data, 0, got);
            off += got;
        }
        closeSync(out);
        await n.fileClose(fd);
        console.log(`extracted ${f} (${off} bytes)`);
    }
    await n.sessionClose(h);
    await n.kernelHalt();
} catch (e) {
    console.error(`extract_from_image: ${e.message ?? e}`);
    rc = 1;
}
// The addon keeps LKL's threads alive; never wait for the event loop.
process.exit(rc);
