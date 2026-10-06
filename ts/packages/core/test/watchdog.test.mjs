import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AnyfsSessionBase, DEFAULT_OP_TIMEOUT_MS } from '../dist/index.js';

const never = () => new Promise(() => {});

/** A session whose ops all go through the base watchdog. `impl` maps an op
 *  name to what the transport does; unlisted ops never answer. */
class GuardedSession extends AnyfsSessionBase {
    constructor(opts, impl = {}) {
        super(opts);
        this.impl = impl;
        this.calls = [];
        this.closedFds = [];
    }
    run(op, ...args) {
        return this.guard(op, () => {
            this.calls.push(op);
            return (this.impl[op] ?? never)(...args);
        });
    }
    async attachBlob() {}
    async attachUrl() {}
    async attachPath() {}
    enter(part) {
        return this.run('enter', part);
    }
    listParts() {
        return this.run('listParts');
    }
    meta() {
        return this.run('meta');
    }
    readdir(p) {
        return this.run('readdir', p);
    }
    stat(p) {
        return this.run('stat', p);
    }
    statFollow(p) {
        return this.run('statFollow', p);
    }
    readlink(p) {
        return this.run('readlink', p);
    }
    realpath(p) {
        return this.run('realpath', p);
    }
    readKernelFile(p) {
        return this.run('readKernelFile', p);
    }
    onProgress() {
        return () => {};
    }
    _openFdRaw(p) {
        return this.run('open', p);
    }
    _readFdRaw(fd, off, n) {
        return this.run('read', fd, off, n);
    }
    _closeFdRaw(fd) {
        this.closedFds.push(fd);
        return this.run('close', fd);
    }
    async _dispose() {
        this.disposedCalled = true;
    }
}

test('the default op watchdog is 60 s', () => {
    assert.equal(DEFAULT_OP_TIMEOUT_MS, 60_000);
    assert.equal(new GuardedSession().opTimeoutMs, 60_000);
});

test('a wedged op rejects and fires onFatal within opTimeoutMs', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 50 });
    const fatals = [];
    s.onFatal((e) => fatals.push(e));
    const t0 = Date.now();
    await assert.rejects(s.readdir('/x'), /readdir timed out after 0\.05s — the engine is wedged/);
    const dt = Date.now() - t0;
    assert.ok(dt >= 45 && dt < 1000, `took ${dt} ms`);
    assert.equal(fatals.length, 1);
    assert.equal(fatals[0].name, 'EngineFatalError');
    assert.match(fatals[0].message, /readdir timed out/);
});

test(
    'after a fatal, ops reject at once without reaching the transport',
    { timeout: 5000 },
    async () => {
        const s = new GuardedSession({ opTimeoutMs: 20 });
        await assert.rejects(s.readdir('/x'));
        const before = s.calls.length;
        await assert.rejects(s.stat('/y'), /readdir timed out/);
        assert.equal(s.calls.length, before);
    },
);

test('an op that answers in time resolves and is not fatal', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 200 }, { readdir: async () => [] });
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    assert.deepEqual(await s.readdir('/'), []);
    await new Promise((r) => setTimeout(r, 250));
    assert.equal(fatal, null);
});

test('ordinary errors pass through and are not fatal', { timeout: 5000 }, async () => {
    const s = new GuardedSession(
        { opTimeoutMs: 200 },
        {
            enter: async () => {
                throw new Error('session_enter failed: rc=-22');
            },
        },
    );
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    await assert.rejects(s.enter(1), /rc=-22/);
    assert.equal(fatal, null);
});

test('opTimeoutMs 0 disables the watchdog', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 0 });
    const r = await Promise.race([
        s.readdir('/').then(
            () => 'settled',
            () => 'settled',
        ),
        new Promise((res) => setTimeout(() => res('pending'), 100)),
    ]);
    assert.equal(r, 'pending');
});

test('close() after a fatal skips fd cleanup so it cannot hang', { timeout: 2000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 30 }, { open: async () => 7 });
    assert.equal(await s.openFd('/f'), 7);
    await assert.rejects(s.readdir('/'));
    await s.close();
    assert.deepEqual(s.closedFds, []);
    assert.equal(s.disposedCalled, true);
});
