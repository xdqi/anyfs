import { test } from 'node:test';
import assert from 'node:assert/strict';
import { caseRecord, finalClass, gate, harnessFailures, selfClass } from '../lib/classify.mjs';
import { matcher } from '../lib/glob.mjs';
import { diffRuns } from '../lib/report.mjs';
import { walkAndRead } from '../lib/walk.mjs';

const rec = (name, cls, extra = {}) => ({
    name,
    class: cls,
    mutation: 'x',
    lastStep: 'walk:0',
    reason: 'r',
    sha256: 'h',
    ...extra,
});

test('classes', () => {
    assert.equal(selfClass({ fatal: null, errors: [] }), 'ok');
    assert.equal(selfClass({ fatal: null, errors: [{}] }), 'error');
    assert.equal(selfClass({ fatal: new Error('x'), errors: [{}] }), 'fatal');
    assert.equal(finalClass({ outcome: { class: 'error' }, timedOut: false }), 'error');
    assert.equal(finalClass({ outcome: null, timedOut: true }), 'hang');
    assert.equal(finalClass({ outcome: null, timedOut: false }), 'crash');
});

test('gate rules', () => {
    assert.equal(gate([rec('a', 'ok'), rec('b', 'error'), rec('c', 'fatal')], 'wasm').pass, true);
    assert.equal(gate([rec('a', 'hang')], 'wasm').pass, false);
    assert.equal(gate([rec('a', 'crash')], 'wasm').pass, false);
    assert.equal(gate([rec('a', 'fatal', { reason: null })], 'wasm').pass, false);
    assert.equal(gate([rec('a-base', 'error', { mutation: 'none' })], 'wasm').pass, false);
    const native = gate([rec('a', 'crash')], 'native');
    assert.equal(native.pass, true); // findings, not failures
    assert.equal(native.problems.length, 1);
});

test('harness failures are cases that died before the image mattered', () => {
    const r = harnessFailures([
        rec('a', 'error', { lastStep: 'boot' }),
        rec('b', 'crash', { lastStep: 'spawn' }),
        rec('c', 'error', { lastStep: 'enter:0' }),
        rec('d', 'ok', { lastStep: 'close' }),
    ]);
    assert.deepEqual(
        r.map((x) => x.name),
        ['a', 'b'],
    );
});

test('--only globs', () => {
    const m = matcher('ext4-*, syz-*');
    assert.ok(m('ext4-base'));
    assert.ok(m('syz-xfs-12345678'));
    assert.ok(!m('ext4panic-base'));
    assert.ok(matcher('vfat-sb-?eserved-sectors')('vfat-sb-reserved-sectors'));
});

test('class flips between runs of the same image are reported', () => {
    const prev = {
        records: [rec('a', 'ok'), rec('b', 'error'), rec('c', 'ok', { sha256: 'old' })],
    };
    const cur = [rec('a', 'fatal'), rec('b', 'error'), rec('c', 'error')];
    assert.deepEqual(diffRuns(prev, cur), [{ name: 'a', was: 'ok', now: 'fatal' }]);
    assert.deepEqual(diffRuns(null, cur), []);
});

function fakeTree() {
    const dirs = {
        '/m': [
            { name: '.', kind: 'dir' },
            { name: '..', kind: 'dir' },
            { name: 'a.txt', kind: 'file' },
            { name: 'bad', kind: 'dir' },
            { name: 'sub', kind: 'dir' },
            { name: 'l', kind: 'link' },
        ],
        '/m/sub': [{ name: 'b.bin', kind: 'file' }],
    };
    const kinds = { bad: 'dir', sub: 'dir', l: 'link' };
    const calls = [];
    return {
        calls,
        async readdir(p) {
            calls.push(['readdir', p]);
            if (p === '/m/bad') throw new Error('EUCLEAN');
            return dirs[p] ?? [];
        },
        async stat(p) {
            calls.push(['stat', p]);
            return { kind: kinds[p.split('/').pop()] ?? 'file' };
        },
        async readlink(p) {
            calls.push(['readlink', p]);
            return 'a.txt';
        },
        async openFd(p) {
            calls.push(['open', p]);
            return 3;
        },
        async readFd(fd, off, n) {
            calls.push(['read', fd, off, n]);
            return new Uint8Array(10);
        },
        async closeFd(fd) {
            calls.push(['close', fd]);
        },
    };
}

test('walk records failing ops and keeps going', async () => {
    const s = fakeTree();
    const r = await walkAndRead(s, '/m');
    assert.equal(r.entries, 5); // a.txt bad sub l b.bin
    assert.equal(r.files, 2);
    assert.equal(r.bytes, 20);
    assert.deepEqual(r.errors, [
        { op: 'readdir', path: '/m/bad', message: 'EUCLEAN', step: 'walk' },
    ]);
    assert.ok(s.calls.some(([op, p]) => op === 'readlink' && p === '/m/l'));
    assert.ok(s.calls.some(([op, , , n]) => op === 'read' && n === 64 * 1024));
});

test('walk honours its limits and stops when told', async () => {
    const limits = { entries: 2, depth: 6, files: 20, readBytes: 1 };
    assert.equal((await walkAndRead(fakeTree(), '/m', { limits })).entries, 2);
    const shallow = await walkAndRead(fakeTree(), '/m', {
        limits: { ...limits, entries: 500, depth: 1 },
    });
    assert.equal(shallow.entries, 4); // sub/ is not descended
    const s = fakeTree();
    const stopped = await walkAndRead(s, '/m', { stopped: () => true });
    assert.equal(stopped.entries, 0);
    assert.deepEqual(s.calls, []);
});

const CASE = {
    name: 'n',
    source: 'generated',
    fs: 'ext4',
    mutation: 'none',
    sha256: 'h',
    expect: { entries: 1 },
};
const RUN = {
    timedOut: false,
    lastStep: 'close',
    code: 0,
    signal: null,
    killedAfterOutcome: false,
};
const out = (cls, extra = {}) => ({
    class: cls,
    lastStep: 'close',
    reason: null,
    failedStep: null,
    errors: [],
    stats: { parts: 1, entries: 1, files: 0, bytes: 0 },
    ...extra,
});

test('caseRecord: ok outcome keeps a null reason', () => {
    const r = caseRecord(CASE, { ...RUN, outcome: out('ok') }, 'wasm');
    assert.equal(r.class, 'ok');
    assert.equal(r.reason, null);
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.expect, { entries: 1 });
});

test('caseRecord: error and fatal outcomes', () => {
    const e = caseRecord(
        CASE,
        { ...RUN, outcome: out('error', { reason: 'enter #0: x', failedStep: 'enter:0' }) },
        'wasm',
    );
    assert.deepEqual([e.class, e.reason, e.failedStep], ['error', 'enter #0: x', 'enter:0']);
    const f = caseRecord(
        CASE,
        {
            ...RUN,
            outcome: out('fatal', { reason: 'boom', failedStep: 'read:0', lastStep: 'read:0' }),
        },
        'wasm',
    );
    assert.deepEqual(
        [f.class, f.reason, f.lastStep, f.failedStep],
        ['fatal', 'boom', 'read:0', 'read:0'],
    );
});

test('caseRecord: no outcome is a hang or a crash', () => {
    const base = { ...RUN, outcome: null, lastStep: 'enter:0', code: null, signal: 'SIGKILL' };
    const h = caseRecord(CASE, { ...base, timedOut: true }, 'wasm');
    assert.deepEqual([h.class, h.lastStep], ['hang', 'enter:0']);
    assert.match(h.reason, /no outcome within/);
    const c = caseRecord(CASE, { ...base, code: 1, signal: null }, 'wasm');
    assert.equal(c.class, 'crash');
    assert.match(c.reason, /exited without an outcome \(code 1/);
});

test('caseRecord: a kill after the outcome keeps the outcome', () => {
    const r = caseRecord(
        CASE,
        { ...RUN, outcome: out('ok'), killedAfterOutcome: true, signal: 'SIGKILL' },
        'native',
    );
    assert.equal(r.class, 'ok');
    assert.equal(r.reason, null);
    assert.equal(r.exit.killedAfterOutcome, true);
});

test('gate: base stats must match expect', () => {
    const base = (stats, extra = {}) =>
        rec('a-base', 'ok', {
            mutation: 'none',
            expect: { entries: 11, files: 5 },
            stats,
            ...extra,
        });
    assert.equal(gate([base({ entries: 11, files: 5 })], 'wasm').pass, true);
    const v = gate([base({ entries: 0, files: 0 })], 'wasm');
    assert.equal(v.pass, false);
    assert.equal(v.problems[0], 'a-base: unmutated base listed 0 entries, expected 11');
    assert.equal(gate([rec('b-base', 'ok', { mutation: 'none' })], 'wasm').pass, true); // no expect
});

test('gate lists a base once', () => {
    const v = gate([rec('a-base', 'hang', { mutation: 'none' })], 'wasm');
    assert.equal(v.problems.length, 1);
});

test('walk skips malformed entry names', async () => {
    const s = fakeTree();
    const rd = s.readdir;
    s.readdir = async (p) =>
        p === '/m'
            ? [
                  { name: '', kind: 'file' },
                  { name: 'x/y', kind: 'file' },
                  { name: 'ok', kind: 'file' },
              ]
            : rd(p);
    const r = await walkAndRead(s, '/m');
    assert.equal(r.entries, 1);
    assert.equal(r.errors.length, 2);
    assert.equal(r.errors[0].op, 'readdir');
    assert.match(r.errors[0].message, /^bad entry name/);
});

test('diffRuns marks flips under a different build', () => {
    const prev = { build: { head: 'a', bundle: '1' }, records: [rec('a', 'ok')] };
    assert.deepEqual(diffRuns(prev, [rec('a', 'fatal')], { head: 'b', bundle: '1' }), [
        { name: 'a', was: 'ok', now: 'fatal', buildChanged: true },
    ]);
    assert.equal(
        diffRuns(prev, [rec('a', 'fatal')], { head: 'a', bundle: '1' })[0].buildChanged,
        false,
    );
});
