#!/usr/bin/env node
/*
 * End-to-end check of the native addon (anyfs_native.node) on disk images:
 * kernel boot, partition metadata, mount, list, stat, extraction to a host
 * file with a content hash, close, repeat, kernel halt.
 *
 * Written for the macOS port (docs/macos.md), but platform-neutral: run it
 * with plain node, or with Electron as node (ELECTRON_RUN_AS_NODE=1
 * <App>.app/Contents/MacOS/<App>), which also proves the addon loads from
 * the packaged app with the app's own Node-API.
 *
 *   native-smoke.mjs --addon PATH --fixtures DIR --expected FILE [--loops N]
 *   native-smoke.mjs --addon PATH --fixtures DIR --write-expected FILE SPEC
 *
 * FILE lists, per image in DIR: the partition table as sessionListJson
 * returns it (every field must match), the partition to mount, directory
 * entries that must be present, and files with their size and sha256. An
 * image whose file is missing from DIR is skipped (the 800 MB Ubuntu qcow2 is
 * optional); --require-all turns that into a failure.
 *
 * --write-expected fills sizes, hashes and partition tables from SPEC (the
 * same shape without them) on a reference platform; the macOS run must then
 * reproduce them exactly.
 *
 * Exit status: 0 all checks passed, 1 a check failed, 2 usage error.
 */
import { createHash } from 'node:crypto';
import { closeSync, existsSync, mkdtempSync, openSync, readFileSync, rmSync, writeFileSync, writeSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const require = createRequire(import.meta.url);
const SESSION_READONLY = 1; // ANYFS_SESSION_READONLY
const MOUNT_RDONLY = 1; // ANYFS_MOUNT_RDONLY
const CHUNK = 1 << 20;

function parseArgs(argv) {
    const o = { loops: 3, requireAll: false, rest: [] };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const next = () => {
            if (i + 1 >= argv.length) usage(`${a} needs a value`);
            return argv[++i];
        };
        if (a === '--addon') o.addon = next();
        else if (a === '--fixtures') o.fixtures = next();
        else if (a === '--expected') o.expected = next();
        else if (a === '--write-expected') o.writeExpected = next();
        else if (a === '--loops') o.loops = Number(next());
        else if (a === '--require-all') o.requireAll = true;
        else o.rest.push(a);
    }
    if (!o.addon || !o.fixtures || !(o.expected || (o.writeExpected && o.rest.length === 1)))
        usage('missing arguments');
    return o;
}

function usage(msg) {
    console.error(`native-smoke: ${msg}`);
    console.error(
        'usage: native-smoke.mjs --addon PATH --fixtures DIR (--expected FILE [--loops N] [--require-all]' +
            ' | --write-expected FILE SPEC)',
    );
    process.exit(2);
}

let failures = 0;
let checks = 0;
function ok(cond, what) {
    checks++;
    if (cond) {
        console.log(`ok   ${what}`);
    } else {
        failures++;
        console.log(`FAIL ${what}`);
    }
    return cond;
}

/* Extract `path` to a host file through fileOpen/pread (the UI's download
 * path) and hash what landed on disk. */
async function extract(n, path, hostFile) {
    const st = JSON.parse(await n.statJson(path));
    const fd = await n.fileOpen(path, 0);
    if (fd < 0) throw new Error(`fileOpen(${path}) = ${fd}`);
    const out = openSync(hostFile, 'w');
    let off = 0;
    try {
        for (;;) {
            const { rc, data } = await n.pread(fd, CHUNK, off);
            if (rc < 0) throw new Error(`pread(${path}, ${off}) = ${rc}`);
            if (rc === 0) break;
            writeSync(out, data, 0, rc);
            off += rc;
        }
    } finally {
        closeSync(out);
        await n.fileClose(fd);
    }
    const sha256 = createHash('sha256').update(readFileSync(hostFile)).digest('hex');
    return { size: st.size, read: off, sha256 };
}

async function runImage(n, fixtures, img, scratch, write) {
    const file = join(fixtures, img.file);
    const h = await n.sessionOpen(file, SESSION_READONLY);
    ok(h >= 0, `${img.file}: sessionOpen`);
    try {
        const meta = JSON.parse(await n.sessionMetaJson(h));
        const parts = JSON.parse(await n.sessionListJson(h));
        if (write) {
            img.meta = meta;
            img.parts = parts;
        } else {
            ok(JSON.stringify(meta) === JSON.stringify(img.meta), `${img.file}: meta ${JSON.stringify(meta)}`);
            ok(parts.length === img.parts.length, `${img.file}: ${parts.length} partitions`);
            for (const want of img.parts) {
                const got = parts.find((p) => p.index === want.index);
                ok(
                    JSON.stringify(got) === JSON.stringify(want),
                    `${img.file}: #${want.index} ${want.fstype || '-'} ${want.label || '-'} ${want.ptype || ''}`,
                );
                if (got && JSON.stringify(got) !== JSON.stringify(want))
                    console.log(`     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`);
            }
        }

        const mnt = await n.sessionEnter(h, img.enter, MOUNT_RDONLY);
        ok(typeof mnt === 'string' && mnt.startsWith('/'), `${img.file}: mount #${img.enter} at ${mnt}`);
        for (const d of img.dirs ?? []) {
            const names = JSON.parse(await n.readdirJson(`${mnt}/${d.path}`)).map((e) => e.name);
            for (const want of d.contains)
                ok(names.includes(want), `${img.file}: /${d.path} lists ${want} (${names.length} entries)`);
        }
        for (const f of img.files) {
            const got = await extract(n, `${mnt}/${f.path}`, join(scratch, 'extract.bin'));
            if (write) {
                f.size = got.size;
                f.sha256 = got.sha256;
                ok(got.read === got.size, `${img.file}: /${f.path} ${got.size} bytes`);
            } else {
                ok(
                    got.size === f.size && got.read === f.size && got.sha256 === f.sha256,
                    `${img.file}: /${f.path} ${got.read}/${got.size} bytes sha256 ${got.sha256.slice(0, 16)}…`,
                );
            }
        }
    } finally {
        ok((await n.sessionClose(h)) === 0, `${img.file}: sessionClose`);
    }
}

async function main() {
    const o = parseArgs(process.argv.slice(2));
    const spec = JSON.parse(readFileSync(o.writeExpected ? o.rest[0] : o.expected, 'utf8'));
    const n = require(resolve(o.addon));
    console.log(`native-smoke: ${process.platform}/${process.arch} node ${process.versions.node}` +
        (process.versions.electron ? ` electron ${process.versions.electron}` : '') + ` addon ${o.addon}`);
    const scratch = mkdtempSync(join(tmpdir(), 'anyfs-native-smoke-'));
    const t0 = Date.now();
    try {
        ok((await n.kernelInit(256, 0)) === 0, 'kernelInit');
        const loops = o.writeExpected ? 1 : o.loops;
        for (let loop = 1; loop <= loops; loop++) {
            console.log(`--- pass ${loop}/${loops}`);
            for (const img of spec.images) {
                if (!existsSync(join(o.fixtures, img.file))) {
                    if (o.requireAll) ok(false, `${img.file}: present in ${o.fixtures}`);
                    else console.log(`skip ${img.file} (not in ${o.fixtures})`);
                    continue;
                }
                await runImage(n, o.fixtures, img, scratch, !!o.writeExpected);
            }
        }
        ok((await n.kernelHalt()) === 0, 'kernelHalt');
    } catch (e) {
        ok(false, `unexpected error: ${e?.stack ?? e}`);
    } finally {
        rmSync(scratch, { recursive: true, force: true });
    }
    if (o.writeExpected) writeFileSync(o.writeExpected, JSON.stringify(spec, null, 2) + '\n');
    console.log(`${failures ? 'FAIL' : 'PASS'} (${checks - failures}/${checks} checks, ${((Date.now() - t0) / 1000).toFixed(1)} s)`);
    // process.exit: the addon keeps LKL's threads; never wait for the loop.
    process.exit(failures ? 1 : 0);
}

await main();
