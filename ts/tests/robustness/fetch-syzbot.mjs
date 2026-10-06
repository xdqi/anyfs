#!/usr/bin/env node
/**
 * Fetch the curated syzbot images (syzbot.json) into
 * ~/.cache/anyfs-robustness/syzbot/, check each download against its pinned
 * sha256, and unpack it read-only. A missing asset or a hash mismatch is an
 * error, never a silent skip. The images are syzbot's: downloaded at run
 * time, never committed or redistributed.
 *   node ts/tests/robustness/fetch-syzbot.mjs [--only <glob>] [--pin]
 * Prints "<case>\t<image path>" per case. --pin (curation only) records the
 * sha256 of entries that have none.
 */
import { createHash } from 'node:crypto';
import {
    chmodSync,
    existsSync,
    mkdirSync,
    readFileSync,
    renameSync,
    rmSync,
    writeFileSync,
} from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { gunzipSync } from 'node:zlib';
import { matcher } from './lib/glob.mjs';
import { SYZBOT_DIR, SYZBOT_MANIFEST } from './lib/paths.mjs';

export const caseName = (e) => `syz-${e.fs}-${e.extid.slice(0, 8)}`;
/** GET with a 120 s timeout; 3 retries (5/15/45 s) on 429, 5xx or a network error. */
async function download(url) {
    for (let attempt = 0; ; attempt++) {
        let failure;
        try {
            const res = await fetch(url, { signal: AbortSignal.timeout(120_000) });
            if (res.ok) return Buffer.from(await res.arrayBuffer());
            if (res.status !== 429 && res.status < 500)
                throw new Error(`${url} → HTTP ${res.status}`);
            failure = `HTTP ${res.status}`;
        } catch (e) {
            if (e.message.includes('→ HTTP')) throw e;
            failure = e.message;
        }
        if (attempt === 3) throw new Error(`${url} → ${failure} (after 3 retries)`);
        await new Promise((r) => setTimeout(r, [5, 15, 45][attempt] * 1000));
    }
}

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

/** Fetch (if needed) the entries `only` selects; return them as cases. */
export async function fetchSyzbot({ only = null, pin = false } = {}) {
    const manifest = JSON.parse(readFileSync(SYZBOT_MANIFEST, 'utf-8'));
    mkdirSync(SYZBOT_DIR, { recursive: true });
    const cases = [];
    let pinned = false;
    for (const e of manifest) {
        const name = caseName(e);
        if (only && !only(name)) continue;
        if (!e.sha256 && !pin) {
            throw new Error(`${name}: no sha256 pinned in syzbot.json (curation: run with --pin)`);
        }
        const img = join(SYZBOT_DIR, `${e.extid}.img`);
        if (!existsSync(img)) {
            const gz = await download(e.url);
            // sha256 pins the downloaded .gz, not the unpacked image.
            const got = sha256(gz);
            if (!e.sha256) {
                e.sha256 = got;
                pinned = true;
            } else if (got !== e.sha256) {
                throw new Error(
                    `${name}: sha256 mismatch for ${e.url}: got ${got}, pinned ${e.sha256}`,
                );
            }
            const tmp = `${img}.tmp`;
            rmSync(tmp, { force: true }); // a stale read-only .tmp would throw EACCES
            writeFileSync(tmp, gunzipSync(gz));
            chmodSync(tmp, 0o444);
            renameSync(tmp, img);
        }
        cases.push({
            name,
            source: 'syzbot',
            fs: e.fs,
            mutation: 'syzbot',
            file: img,
            sha256: e.sha256,
            extid: e.extid,
            title: e.title,
            link: e.link,
        });
    }
    if (pinned) writeFileSync(SYZBOT_MANIFEST, `${JSON.stringify(manifest, null, 4)}\n`);
    return cases;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
    const { values } = parseArgs({
        options: { only: { type: 'string' }, pin: { type: 'boolean', default: false } },
    });
    const cases = await fetchSyzbot({
        only: values.only ? matcher(values.only) : null,
        pin: values.pin,
    });
    for (const c of cases) console.log(`${c.name}\t${c.file}`);
}
