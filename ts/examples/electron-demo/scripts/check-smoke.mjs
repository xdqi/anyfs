#!/usr/bin/env node
// Assert a native smoke report (src/native-smoke.ts) against the fixture's
// expected.json (make-smoke-fixture.sh). Fails on anything that would mean
// the package did not run on its own native addon: dev-path addon, missing
// renderer or wasm fallback, wrong partitions, wrong bytes, no clean halt.
//
// Usage: node check-smoke.mjs <report.json> <expected.json>
import { readFileSync } from 'node:fs';
import { relative, isAbsolute } from 'node:path';

const [reportPath, expectedPath] = process.argv.slice(2);
if (!reportPath || !expectedPath) {
    console.error('usage: check-smoke.mjs <report.json> <expected.json>');
    process.exit(2);
}
const r = JSON.parse(readFileSync(reportPath, 'utf8'));
const x = JSON.parse(readFileSync(expectedPath, 'utf8'));
const errors = [];
const check = (cond, msg) => {
    if (!cond) errors.push(msg);
};

const inside = (child, parent) => {
    if (!child || !parent) return false;
    const rel = relative(parent, child);
    return rel !== '' && !rel.startsWith('..') && !isAbsolute(rel);
};

check(r.ok === true, `smoke failed: ${r.error ?? 'ok is not true'}`);
check(r.packaged === true, 'app.isPackaged is false (not running the packaged app)');
check(
    inside(r.addonPath, r.resourcesPath),
    `addon ${r.addonPath} is not the staged one under ${r.resourcesPath}`,
);
check(r.rendererIndex === true, `no index.html in renderer dir ${r.rendererDir}`);
check(
    (r.wasmBundle ?? []).some((f) => /^wasm\/[0-9a-f]{16}\/anyfs\.wasm$/.test(f)),
    'wasm fallback (wasm/<hash>/anyfs.wasm) missing from the renderer',
);
for (const p of x.partitions) {
    check(
        (r.partitions ?? []).some((q) => q.fstype === p.fstype && q.label === p.label),
        `partition ${p.fstype}/${p.label} not listed`,
    );
}
check(r.mounted?.label === x.part, `mounted ${r.mounted?.label}, expected ${x.part}`);
for (const e of x.entries) check((r.entries ?? []).includes(e), `${e} not in the mounted root`);
check(r.file?.path === x.read, `read ${r.file?.path}, expected ${x.read}`);
check(
    r.file?.size === x.size && r.file?.read === x.size,
    `size/read ${r.file?.size}/${r.file?.read}, expected ${x.size}`,
);
check(r.file?.sha256 === x.sha256, `sha256 ${r.file?.sha256}, expected ${x.sha256}`);
check(r.kernelHalt === 0, `kernelHalt returned ${r.kernelHalt}`);

console.log(
    `check-smoke: ${r.platform}/${r.arch} electron ${r.versions?.electron}, addon ${r.addonPath}`,
);
console.log(
    `check-smoke: partitions ${(r.partitions ?? []).map((p) => `${p.index}:${p.fstype ?? '-'}/${p.label ?? '-'}`).join(' ')}`,
);
if (errors.length) {
    for (const e of errors) console.error(`check-smoke: FAIL ${e}`);
    process.exit(1);
}
console.log(`check-smoke: OK (${x.read} sha256 ${r.file.sha256})`);
