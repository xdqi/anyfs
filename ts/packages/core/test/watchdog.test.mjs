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
    kill(err) {
        this.fireFatal(err);
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
    await new Promise((r) => setTimeout(r, 250));
    assert.equal(fatal, null);
});

const flush = async () => {
    for (let i = 0; i < 5; i++) await Promise.resolve();
};

for (const [label, opTimeoutMs, ticks] of [
    ['0', 0, DEFAULT_OP_TIMEOUT_MS + 1],
    ['Infinity', Infinity, DEFAULT_OP_TIMEOUT_MS + 1],
    ['2 ** 31 (capped, not clamped to 1 ms)', 2 ** 31, 1000],
]) {
    test(`opTimeoutMs ${label} does not fire early`, (t) => {
        t.mock.timers.enable({ apis: ['setTimeout'] });
        const s = new GuardedSession({ opTimeoutMs });
        let fatal = null;
        let settled = false;
        s.onFatal((e) => (fatal = e));
        s.readdir('/').then(
            () => (settled = true),
            () => (settled = true),
        );
        t.mock.timers.tick(ticks);
        return flush().then(() => {
            assert.equal(settled, false);
            assert.equal(fatal, null);
        });
    });
}

test(
    'a synchronous throw from the transport rejects the op and is not fatal',
    { timeout: 5000 },
    async () => {
        const s = new GuardedSession(
            { opTimeoutMs: 200 },
            {
                readdir: () => {
                    throw new Error('sync boom');
                },
            },
        );
        let fatal = null;
        s.onFatal((e) => (fatal = e));
        await assert.rejects(s.readdir('/'), /sync boom/);
        await new Promise((r) => setTimeout(r, 250));
        assert.equal(fatal, null);
    },
);

test('in-flight ops reject as soon as the session goes fatal', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 100 });
    const a = s.run('a').then(
        () => null,
        (e) => ({ e, at: Date.now() }),
    );
    await new Promise((r) => setTimeout(r, 60));
    const b = s.run('b').then(
        () => null,
        (e) => ({ e, at: Date.now() }),
    );
    const [ra, rb] = await Promise.all([a, b]);
    assert.match(ra.e.message, /a timed out/);
    assert.equal(rb.e, ra.e);
    assert.ok(rb.at - ra.at < 40, `b settled ${rb.at - ra.at} ms after a`);
});

test('fireFatal rejects pending ops even with the watchdog off', { timeout: 5000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 0 });
    const p = s.readdir('/');
    const err = new Error('x');
    s.kill(err);
    await assert.rejects(p, (e) => e === err);
});

test('close() after a fatal skips fd cleanup so it cannot hang', { timeout: 2000 }, async () => {
    const s = new GuardedSession({ opTimeoutMs: 30 }, { open: async () => 7 });
    assert.equal(await s.openFd('/f'), 7);
    await assert.rejects(s.readdir('/'));
    await s.close();
    assert.deepEqual(s.closedFds, []);
    assert.equal(s.disposedCalled, true);
});
