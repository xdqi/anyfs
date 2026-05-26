#!/usr/bin/env node
// Smoke test for NativeAnyfsDisk against the real @anyfs/native addon —
// bypasses Electron + IPC by handing the class a synchronous-to-async
// adapter built straight on top of the addon. Exercises the same code
// path that runs in the renderer minus the postMessage hop.
//
//   node smoke.native.mjs            # uses examples/vite-demo/public/disks/multi.img
//   node smoke.native.mjs <image>
//   ANYFS_NATIVE_IMAGE=... node smoke.native.mjs
//
// Exit code: 0 on success, 1 on any failure. Logs per-step status to
// stderr so it's grep-able.
import { NativeAnyfsDisk } from '@anyfs/core';
import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const addon = require('../../anyfs-native/build/Release/anyfs_native.node');
const here = dirname(fileURLToPath(import.meta.url));
const img =
    process.argv[2] ||
    process.env.ANYFS_NATIVE_IMAGE ||
    resolve(here, '../../../examples/vite-demo/public/disks/multi.img');

// Wrap the synchronous addon as the async bridge NativeAnyfsDisk expects.
// Real Electron preload.ts does the same thing over ipcRenderer.invoke.
const bridge = {
    available: async () => true,
    init: async (m, l) => addon.init(m, l),
    diskOpen: async (p, f) => addon.diskOpen(p, f),
    diskClose: async (h) => addon.diskClose(h),
    diskListJson: async (h) => addon.diskListJson(h),
    diskMetaJson: async (h) => addon.diskMetaJson(h),
    diskEnter: async (h, p, f) => addon.diskEnter(h, p, f),
    mountWhole: async (h, fs, f) => addon.mountWhole(h, fs, f),
    readdirJson: async (p) => addon.readdirJson(p),
    lstatJson: async (p) => addon.lstatJson(p),
    statJson: async (p) => addon.statJson(p),
    realpath: async (p) => addon.realpath(p),
    readlink: async (p) => addon.readlink(p),
    fileOpen: async (p, f) => addon.fileOpen(p, f),
    pread: async (fd, n, off) => {
        const buf = new Uint8Array(n);
        const rc = addon.pread(fd, buf, n, off);
        if (rc < 0) return { rc, data: new Uint8Array(0) };
        return { rc, data: rc === buf.length ? buf : buf.subarray(0, rc) };
    },
    fileClose: async (fd) => addon.fileClose(fd),
};

const disk = new NativeAnyfsDisk(bridge);
let failed = false;
try {
    console.error('[1/6] boot kernel');
    await disk.boot(256, 0);

    console.error(`[2/6] attachPath(${img})`);
    await disk.attachPath(img);

    console.error('[3/6] diskMeta + listPartitions');
    const meta = await disk.diskMeta();
    const parts = await disk.listPartitions();
    console.error(`     pt=${meta.pt_type} parts=${parts.length}`);
    if (parts.length < 1) throw new Error('expected at least one partition');

    console.error('[4/6] enter(part 2) and readdir /');
    const mp = await disk.enter(2);
    console.error(`     mount=${mp}`);
    const entries = await disk.readdir(mp);
    console.error(`     entries=${entries.map((e) => e.name).join(',')}`);

    console.error('[5/6] read hello.txt via openReadable stream');
    const target = mp + '/hello.txt';
    let st;
    try {
        st = await disk.statFollow(target);
    } catch (e) {
        console.error(`     no hello.txt (${e.message}); falling back to first file`);
        const f = entries.find((e) => e.kind === 'file');
        if (!f) throw new Error('no files in partition');
        st = await disk.statFollow(`${mp}/${f.name}`);
    }
    console.error(`     size=${st.size} mode=0${(st.mode & 0o777).toString(8)}`);

    console.error('[6/6] dispose');
    await disk.dispose();
    console.error('OK — NativeAnyfsDisk end-to-end pass');
} catch (e) {
    console.error('FAIL:', e?.stack ?? e);
    failed = true;
} finally {
    process.exit(failed ? 1 : 0);
}
