#!/usr/bin/env node
/**
 * Fetch the REAL remote disk-image fixtures the anyfs-reader E2E suite browses
 * against downloaded (rather than generated) inputs:
 *
 *   - trusty-cloud.qcow2  Ubuntu 14.04 cloud image (qcow2), ~250 MB
 *   - trusty.iso          Ubuntu 14.04.6 server install ISO (iso9660), ~600 MB
 *
 * Both are large, so every download is STREAMED to disk (never buffered whole in
 * memory) and the sha256 is computed by streaming the file through the hash.
 *
 * Each download lands on a `.part` temp file and is only renamed to its final
 * name once the stream has fully drained. An interrupted run therefore leaves a
 * `.part` (re-fetched next run), never a truncated file that looks complete.
 *
 * The script is idempotent: a file that already exists is hashed + reported and
 * its download is skipped. On success it prints the exact byte size + sha256 of
 * every image so the manifest (manifest.ts) guards can be reconciled.
 */
import { createReadStream, createWriteStream, existsSync, renameSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const IMAGES_DIR = resolve(here, 'images');

/** Remote fixtures to fetch, with their final on-disk paths. */
const DOWNLOADS = [
    {
        file: resolve(IMAGES_DIR, 'trusty-cloud.qcow2'),
        url: 'https://cloud-images.ubuntu.com/trusty/current/trusty-server-cloudimg-amd64-disk1.img',
    },
    {
        file: resolve(IMAGES_DIR, 'trusty.iso'),
        url: 'https://releases.ubuntu.com/trusty/ubuntu-14.04.6-server-amd64.iso',
    },
];

/** sha256 of a file, computed by streaming it through the hash (no full read). */
async function sha256(file) {
    const hash = createHash('sha256');
    await pipeline(createReadStream(file), hash);
    return hash.digest('hex');
}

/** Download `url` to `file`, streaming to a temp file then renaming on success. */
async function download(url, file) {
    const tmp = `${file}.part`;
    const res = await fetch(url);
    if (!res.ok) {
        throw new Error(`fetch ${url} failed: HTTP ${res.status} ${res.statusText}`);
    }
    if (!res.body) {
        throw new Error(`fetch ${url} returned no body`);
    }
    await pipeline(Readable.fromWeb(res.body), createWriteStream(tmp));
    renameSync(tmp, file);
}

async function main() {
    for (const { file, url } of DOWNLOADS) {
        if (existsSync(file)) {
            const { size } = statSync(file);
            const digest = await sha256(file);
            console.log(`cached  ${file}`);
            console.log(`  size  ${size}`);
            console.log(`  sha256 ${digest}`);
            continue;
        }
        console.log(`fetch   ${url}`);
        console.log(`     -> ${file}`);
        await download(url, file);
        const { size } = statSync(file);
        const digest = await sha256(file);
        console.log(`  size  ${size}`);
        console.log(`  sha256 ${digest}`);
    }
}

main().catch((err) => {
    console.error(err);
    process.exitCode = 1;
});
