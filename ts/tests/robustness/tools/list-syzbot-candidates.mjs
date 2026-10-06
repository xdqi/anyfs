#!/usr/bin/env node
/**
 * One-off curation helper for syzbot.json. Lists syzbot bugs on filesystems
 * anyfs supports whose title points at a read path (mount, lookup, readdir,
 * read) and that ship a "mounted in repro" image. Prints TSV:
 *   subsystem  list  extid  sb_errors  asset  title
 * sb_errors is the ext4 superblock's errors behaviour read from the image
 * (3 = panic); "-" for other filesystems. One request per second.
 *   node ts/tests/robustness/tools/list-syzbot-candidates.mjs [subsystem...]
 */
import { gunzipSync } from 'node:zlib';

const DASH = 'https://syzkaller.appspot.com';
const SUBSYSTEMS = [
    'ext4',
    'btrfs',
    'xfs',
    'fat',
    'hfs',
    'isofs',
    'udf',
    'ntfs3',
    'f2fs',
    'squashfs',
    'exfat',
];
const READ_PATH =
    /mount|fill_super|lookup|readdir|iterate|iget|find_entry|search_dir|get_block|map_blocks|bmap|read|getattr|statfs|get_link|listxattr|xattr_list/i;
const WRITE_PATH =
    /write|setattr|truncate|rename|unlink|fallocate|dirty|create|mkdir|evict|sync|discard|remount|resize|balance|quota|commit|punch|orphan|delete|free_blocks|alloc/i;
const PER_LIST = Number(process.env.PER_LIST ?? 12);
const ASSET = /https:\/\/storage\.googleapis\.com\/syzbot-assets\/[0-9a-f]+\/mount_\d+\.gz/;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function get(url) {
    for (let wait = 30_000; ; wait *= 2) {
        await sleep(1000);
        const res = await fetch(url);
        if (res.status === 429 && wait <= 240_000) {
            await sleep(wait);
            continue;
        }
        if (!res.ok) throw new Error(`${url} → HTTP ${res.status}`);
        return res;
    }
}

function bugs(html) {
    const out = [];
    const re = /href="\/bug\?extid=([0-9a-f]+)">([^<]+)</g;
    for (let m = re.exec(html); m; m = re.exec(html)) {
        out.push({ extid: m[1], title: m[2].replace(/&#39;/g, "'").replace(/&amp;/g, '&') });
    }
    return out;
}

const subsystems = process.argv.length > 2 ? process.argv.slice(2) : SUBSYSTEMS;
console.log(['subsystem', 'list', 'extid', 'sb_errors', 'asset', 'title'].join('\t'));
for (const s of subsystems) {
    for (const [list, url] of [
        ['open', `${DASH}/upstream/s/${s}`],
        ['fixed', `${DASH}/upstream/fixed?label=subsystems:${s}`],
    ]) {
        let html;
        try {
            html = await (await get(url)).text();
        } catch (e) {
            console.error(`# ${s} ${list}: ${e.message}`);
            continue;
        }
        const picks = bugs(html)
            .filter((b) => READ_PATH.test(b.title) && !WRITE_PATH.test(b.title))
            .slice(0, PER_LIST);
        for (const b of picks) {
            const asset = ASSET.exec(await (await get(`${DASH}/bug?extid=${b.extid}`)).text())?.[0];
            if (!asset) continue;
            let sbErrors = '-';
            if (s === 'ext4') {
                const img = gunzipSync(Buffer.from(await (await get(asset)).arrayBuffer()));
                if (img.length >= 2048 && img.readUInt16LE(1024 + 56) === 0xef53) {
                    sbErrors = String(img.readUInt16LE(1024 + 60));
                }
            }
            console.log([s, list, b.extid, sbErrors, asset, b.title].join('\t'));
        }
    }
}
