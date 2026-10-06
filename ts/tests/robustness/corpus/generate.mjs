#!/usr/bin/env node
/**
 * Build the robustness corpus: rootless base images of a known tree, plus
 * deterministic mutations of them.
 *   node ts/tests/robustness/corpus/generate.mjs [--force]
 * Writes ~/.cache/anyfs-robustness/generated/<case>.<ext> — read-only, so a
 * run can't modify a case — and cases.json. Does nothing when cases.json and
 * every image already exist, unless --force.
 */
import { createHash } from 'node:crypto';
import {
    chmodSync,
    closeSync,
    existsSync,
    ftruncateSync,
    mkdirSync,
    openSync,
    readFileSync,
    readdirSync,
    renameSync,
    rmSync,
    writeFileSync,
    writeSync,
} from 'node:fs';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { CASES_JSON, GENERATED_DIR } from '../lib/paths.mjs';
import { BASES, TOOLS, buildAllBases } from './bases.mjs';
import { mutationCases } from './mutations.mjs';
import { requireTools } from './tools.mjs';

const CHUNK = 64 * 1024;
const ZERO = Buffer.alloc(CHUNK);
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

/** Write `buf` to `file` (read-only), leaving all-zero chunks as holes. */
function writeCase(file, buf) {
    rmSync(file, { force: true });
    const fd = openSync(file, 'w');
    try {
        ftruncateSync(fd, buf.length);
        for (let off = 0; off < buf.length; off += CHUNK) {
            const chunk = buf.subarray(off, Math.min(off + CHUNK, buf.length));
            if (!chunk.equals(ZERO.subarray(0, chunk.length))) {
                writeSync(fd, chunk, 0, chunk.length, off);
            }
        }
    } finally {
        closeSync(fd);
    }
    chmodSync(file, 0o444);
}

/**
 * What a walk of each unmutated base must see (a base the harness lists as
 * empty has silently stopped testing anything). Counts come from the tree in
 * bases.mjs: ext* and qcow2/vmdk have lost+found (11 entries), vfat has none
 * (9), btrfs/xfs/iso9660/squashfs have none (10). gpt/mbr/mbrext hold the
 * ext4 and vfat bases in two partitions (11 + 9). exfat, f2fs and ntfs are
 * formatted empty, so only their mount is checked.
 */
const TREE = { files: 5, bytes: 71730 };
const BASE_EXPECT = {
    ext4: { parts: 1, entries: 11, ...TREE },
    ext4panic: { parts: 1, entries: 11, ...TREE },
    ext2: { parts: 1, entries: 11, ...TREE },
    vfat: { parts: 1, entries: 9, ...TREE },
    exfat: { parts: 1, entries: 0 },
    f2fs: { parts: 1, entries: 0 },
    ntfs: { parts: 1, entries: 0 },
    btrfs: { parts: 1, entries: 10, ...TREE },
    xfs: { parts: 1, entries: 10, ...TREE },
    iso9660: { parts: 1, entries: 10, ...TREE },
    squashfs: { parts: 1, entries: 10, ...TREE },
    qcow2: { parts: 1, entries: 11, ...TREE },
    vmdk: { parts: 1, entries: 11, ...TREE },
    gpt: { parts: 2, entries: 20, files: 10, bytes: 143460 },
    mbr: { parts: 2, entries: 20, files: 10, bytes: 143460 },
    mbrext: { parts: 2, entries: 20, files: 10, bytes: 143460 },
};

/** sha256 over corpus/*.mjs (by name) and the sorted case names: a changed
 *  builder or mutation list invalidates a generated corpus. */
function fingerprint() {
    const dir = dirname(fileURLToPath(import.meta.url));
    const h = createHash('sha256');
    for (const f of readdirSync(dir)
        .filter((n) => n.endsWith('.mjs') && !n.startsWith('.'))
        .sort()) {
        h.update(`${f}\0`)
            .update(readFileSync(join(dir, f)))
            .update('\0');
    }
    const names = [
        ...Object.keys(BASES).map((b) => `${b}-base`),
        ...mutationCases().map((m) => m.name),
    ].sort();
    return h.update(names.join('\n')).digest('hex');
}

function upToDate(fp) {
    try {
        const j = JSON.parse(readFileSync(CASES_JSON, 'utf-8'));
        return j.fingerprint === fp && j.cases.every((c) => existsSync(c.file));
    } catch {
        return false; // missing or unparsable
    }
}

function main() {
    const { values } = parseArgs({ options: { force: { type: 'boolean', default: false } } });
    const fp = fingerprint();
    if (!values.force && upToDate(fp)) {
        console.log(`corpus up to date: ${CASES_JSON}`);
        return;
    }
    requireTools(TOOLS);
    mkdirSync(GENERATED_DIR, { recursive: true });
    // Stale until the new set is complete: an interrupted run must not look done.
    rmSync(CASES_JSON, { force: true });
    const scratch = join(GENERATED_DIR, '.scratch');
    rmSync(scratch, { recursive: true, force: true });
    const { bufs, layouts } = buildAllBases(scratch, { log: (n) => console.log(`base  ${n}`) });

    const cases = [];
    const emit = (name, base, mutation, buf) => {
        const b = BASES[base];
        const file = join(GENERATED_DIR, `${name}.${b.ext}`);
        writeCase(file, buf);
        cases.push({
            name,
            source: 'generated',
            base,
            fs: b.fs,
            mutation,
            file,
            sha256: sha256(buf),
            ...(mutation === 'none' ? { expect: BASE_EXPECT[base] } : {}),
        });
    };
    for (const base of Object.keys(BASES)) emit(`${base}-base`, base, 'none', bufs[base]);
    for (const m of mutationCases()) {
        emit(m.name, m.base, m.mutation, m.apply(bufs[m.base], layouts[m.base]));
        console.log(`case  ${m.name}`);
    }
    rmSync(scratch, { recursive: true, force: true });
    // Prune images of cases that no longer exist.
    const keep = new Set(['cases.json', ...cases.map((c) => basename(c.file))]);
    for (const f of readdirSync(GENERATED_DIR)) {
        if (!keep.has(f)) rmSync(join(GENERATED_DIR, f), { recursive: true, force: true });
    }
    const doc = { generatedAt: new Date().toISOString(), fingerprint: fp, cases };
    writeFileSync(`${CASES_JSON}.tmp`, `${JSON.stringify(doc, null, 4)}\n`);
    renameSync(`${CASES_JSON}.tmp`, CASES_JSON);
    console.log(`${cases.length} cases → ${CASES_JSON}`);
}

main();
