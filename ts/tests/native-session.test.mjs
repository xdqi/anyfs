/**
 * Native addon session test — exercises the renamed N-API surface:
 *   kernelInit / sessionOpen / sessionClose / sessionListJson /
 *   sessionMetaJson / sessionEnter / readdirJson / kernelHalt
 *
 * Requires a disk image with at least one partition.
 *
 * Usage:
 *   IMAGE=/path/to/disk.img node ts/tests/native-session.test.mjs
 *
 * Default image if IMAGE is not set:   /home/kosaka/debian-13.5.0-amd64-netinst.iso
 */

import { createRequire } from 'module';
import { ok, strictEqual } from 'node:assert';

const require = createRequire(import.meta.url);
const addon = require('../packages/anyfs-native/build/Release/anyfs_native.node');

const IMAGE = process.env.IMAGE || '/home/kosaka/debian-13.5.0-amd64-netinst.iso';

let passed = 0;
let failed = 0;

function test(name, fn) {
    try {
        fn();
        passed++;
        console.log(`  ✓ ${name}`);
    } catch (err) {
        failed++;
        console.error(`  ✗ ${name}: ${err.message}`);
    }
}

function assert(cond, msg) {
    ok(cond, msg);
}

console.log(`=== anyfs-native session test ===`);
console.log(`Image: ${IMAGE}`);
console.log(`Exports: ${Object.keys(addon).sort().join(', ')}\n`);

// ── 1. Init ───────────────────────────────────────

let handle = -1;

test('kernelInit(256, 7) returns 0', () => {
    const rc = addon.kernelInit(256, 7);
    strictEqual(rc, 0);
});

// ── 2. Session open ───────────────────────────────

test('sessionOpen(path, 1) returns handle >= 0', () => {
    handle = addon.sessionOpen(IMAGE, 1);
    assert(handle >= 0, `handle=${handle} expected >= 0`);
});

// ── 3. List parts ─────────────────────────────────

test('sessionListJson returns array', () => {
    const json = addon.sessionListJson(handle);
    const list = JSON.parse(json);
    assert(Array.isArray(list), 'expected array');
    console.log(`    ${list.length} partition entries`);
    for (const e of list.slice(0, 5)) {
        console.log(
            `      idx=${e.index} slot=${e.slot_id} kind=${e.kind} ` +
                `fstype=${e.fstype || '?'} label="${e.label || ''}"`,
        );
    }
});

// ── 4. Meta ───────────────────────────────────────

test('sessionMetaJson returns expected shape', () => {
    const json = addon.sessionMetaJson(handle);
    const meta = JSON.parse(json);
    assert(typeof meta.logical_size === 'number', 'expected logical_size');
    assert(typeof meta.pt_type === 'string', 'expected pt_type');
    console.log(`    logical_size=${meta.logical_size} pt_type=${meta.pt_type}`);
});

// ── 5. Enter partition ──────────────────────

test('sessionEnter(handle, 2, 0) mounts partition', () => {
    const mp = addon.sessionEnter(handle, 2, 0);
    assert(mp && mp.length > 0, `mount path empty: "${mp}"`);
    console.log(`    mount path: "${mp}"`);

    // readdir the mount point
    const json = addon.readdirJson(mp);
    const entries = JSON.parse(json);
    assert(Array.isArray(entries), 'expected entries array');
    console.log(`    ${entries.length} root entries`);
    for (const f of entries.slice(0, 5)) {
        console.log(`      ${f.name} (${f.kind})`);
    }
});

// ── 6. Close ──────────────────────────────────────

test('sessionClose(handle) returns 0', () => {
    const rc = addon.sessionClose(handle);
    strictEqual(rc, 0);
    handle = -1;
});

// ── 7. Halt ───────────────────────────────────────

test('kernelHalt succeeds', () => {
    addon.kernelHalt();
    // kernelHalt is void
    assert(true, 'kernelHalt completed');
});

// ── Summary ───────────────────────────────────────

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
