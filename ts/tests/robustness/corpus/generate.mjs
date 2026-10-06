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
    rmSync,
    writeFileSync,
    writeSync,
} from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
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

function upToDate() {
    if (!existsSync(CASES_JSON)) return false;
    const { cases } = JSON.parse(readFileSync(CASES_JSON, 'utf-8'));
    return cases.every((c) => existsSync(c.file));
}

function main() {
    const { values } = parseArgs({ options: { force: { type: 'boolean', default: false } } });
    if (!values.force && upToDate()) {
        console.log(`corpus up to date: ${CASES_JSON}`);
        return;
    }
    requireTools(TOOLS);
    mkdirSync(GENERATED_DIR, { recursive: true });
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
        });
    };
    for (const base of Object.keys(BASES)) emit(`${base}-base`, base, 'none', bufs[base]);
    for (const m of mutationCases()) {
        emit(m.name, m.base, m.mutation, m.apply(bufs[m.base], layouts[m.base]));
        console.log(`case  ${m.name}`);
    }
    rmSync(scratch, { recursive: true, force: true });
    writeFileSync(
        CASES_JSON,
        `${JSON.stringify({ generatedAt: new Date().toISOString(), cases }, null, 4)}\n`,
    );
    console.log(`${cases.length} cases → ${CASES_JSON}`);
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) main();
