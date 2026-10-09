#!/usr/bin/env node
// Assert a drives smoke report (ANYFS_DRIVES_SMOKE=1 in src/main.ts): the
// packaged drivelist addon loaded from resources/native/ and listed the
// host's disks with the fork's partition fields. Every CI runner has a
// mounted system disk, so at least one partition must carry a filesystem
// type and a mountpoint.
//
// Usage: node check-drives.mjs <drives-report.json>
import { readFileSync } from 'node:fs';
import { isAbsolute, relative } from 'node:path';

const [reportPath] = process.argv.slice(2);
if (!reportPath) {
    console.error('usage: check-drives.mjs <drives-report.json>');
    process.exit(2);
}
const r = JSON.parse(readFileSync(reportPath, 'utf8'));
const errors = [];
const check = (cond, msg) => {
    if (!cond) errors.push(msg);
};
const inside = (child, parent) => {
    if (!child || !parent) return false;
    const rel = relative(parent, child);
    return rel !== '' && !rel.startsWith('..') && !isAbsolute(rel);
};
const PART_FIELDS = {
    device: 'string',
    size: 'number|null',
    number: 'number|null',
    fstype: 'string|null',
    label: 'string|null',
    uuid: 'string|null',
    partlabel: 'string|null',
    parttype: 'string|null',
    isReadOnly: 'boolean',
};
const typeOk = (v, spec) =>
    spec.split('|').some((t) => (t === 'null' ? v === null : typeof v === t));

check(r.ok === true, `drives smoke failed: ${r.error ?? 'ok is not true'}`);
check(
    inside(r.drivelistPath, r.resourcesPath),
    `drivelist ${r.drivelistPath} is not the staged one under ${r.resourcesPath}`,
);
const drives = Array.isArray(r.drives) ? r.drives : [];
check(drives.length > 0, 'no drives listed');
const parts = [];
for (const d of drives) {
    check(
        typeof d.device === 'string' && d.device !== '',
        `drive without a device: ${JSON.stringify(d)}`,
    );
    check('partitions' in d, `${d.device}: no partitions field (not the anyfs fork?)`);
    for (const p of d.partitions ?? []) {
        parts.push(p);
        for (const [k, spec] of Object.entries(PART_FIELDS)) {
            check(
                typeOk(p[k], spec),
                `${p.device ?? d.device}: ${k}=${JSON.stringify(p[k])} is not ${spec}`,
            );
        }
        check(Array.isArray(p.mountpoints), `${p.device}: mountpoints is not an array`);
    }
}
check(parts.length > 0, 'no drive reported partitions');
check(
    parts.some((p) => p.fstype && p.mountpoints?.length > 0),
    'no partition with both a filesystem type and a mountpoint',
);

for (const d of drives) {
    const ps = (d.partitions ?? [])
        .map(
            (p) =>
                `${p.device} ${p.fstype ?? '-'}/${p.label ?? '-'} ${(p.mountpoints ?? []).map((m) => m.path).join(',')}`,
        )
        .join('; ');
    console.log(
        `check-drives: ${d.device} ${d.size ?? '?'} B ${d.partitionTableType ?? '-'} [${ps}]`,
    );
}
if (errors.length) {
    for (const e of errors) console.error(`check-drives: FAIL ${e}`);
    process.exit(1);
}
console.log(`check-drives: OK (${drives.length} drives, ${parts.length} partitions)`);
