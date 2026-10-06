import { test } from 'node:test';
import assert from 'node:assert/strict';
import { finalClass, gate, harnessFailures, selfClass } from '../lib/classify.mjs';
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
    assert.deepEqual(r.errors, [{ op: 'readdir', path: '/m/bad', message: 'EUCLEAN' }]);
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
