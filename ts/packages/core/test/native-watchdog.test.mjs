import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NativeSession, prewarmNative } from '../dist/index.js';

const never = () => new Promise(() => {});
const OPS = [
    'available',
    'init',
    'diskOpen',
    'diskClose',
    'diskListJson',
    'diskMetaJson',
    'diskEnter',
    'readdirJson',
    'lstatJson',
    'statJson',
    'realpath',
    'readlink',
    'startProxy',
    'stopProxy',
    'fileOpen',
    'pread',
    'fileClose',
];

/** A bridge whose ops answer from `impl`, or never. */
function fakeBridge(impl = {}) {
    const calls = [];
    const bridge = {};
    for (const name of OPS) {
        bridge[name] = (...args) => {
            calls.push(name);
            return (impl[name] ?? never)(...args);
        };
    }
    if (impl.onFatal) bridge.onFatal = impl.onFatal;
    return { bridge, calls };
}

test(
    'a wedged readdir rejects and fires onFatal within opTimeoutMs',
    { timeout: 5000 },
    async () => {
        const { bridge, calls } = fakeBridge({ diskOpen: async () => 0 });
        const s = new NativeSession(bridge, { opTimeoutMs: 50 });
        await s.attachPath('/img');
        const fatals = [];
        s.onFatal((e) => fatals.push(e.message));
        await assert.rejects(
            s.readdir('/lklmnt/a'),
            /readdir timed out after 0\.05s — the engine is wedged/,
        );
        assert.equal(fatals.length, 1);
        const n = calls.length;
        await assert.rejects(s.stat('/lklmnt/a'), /readdir timed out/);
        assert.equal(calls.length, n);
    },
);

test(
    'the timer starts when an op reaches the engine, not while it queues',
    { timeout: 5000 },
    async () => {
        const { bridge } = fakeBridge({
            diskOpen: async () => 0,
            readdirJson: () => new Promise((r) => setTimeout(() => r('[]'), 80)),
            lstatJson: () =>
                new Promise((r) => setTimeout(() => r(JSON.stringify({ ino: 1 })), 60)),
        });
        const s = new NativeSession(bridge, { opTimeoutMs: 100 });
        await s.attachPath('/img');
        const a = s.readdir('/a'); // 80 ms
        const b = s.stat('/b'); // queued 80 ms, then 60 ms: 140 ms in total, 60 ms on the engine
        assert.deepEqual(await a, []);
        assert.deepEqual(await b, { ino: 1 });
    },
);

test('a host-reported engine failure still fires onFatal', () => {
    let push;
    const { bridge } = fakeBridge({
        onFatal: (cb) => {
            push = cb;
            return () => {};
        },
    });
    const s = new NativeSession(bridge);
    const fatals = [];
    s.onFatal((e) => fatals.push([e.name, e.message]));
    push('QEMU thread did not finish read within 120000 ms');
    assert.deepEqual(fatals, [
        [
            'EngineFatalError',
            'anyfs-native engine failed: QEMU thread did not finish read within 120000 ms',
        ],
    ]);
});

const tick = () => new Promise((r) => setImmediate(r));

test('close() after a watchdog fatal does not touch the engine', { timeout: 2000 }, async () => {
    const { bridge, calls } = fakeBridge({ diskOpen: async () => 0, fileOpen: async () => 5 });
    const s = new NativeSession(bridge, { opTimeoutMs: 30 });
    await s.attachPath('/img');
    assert.equal(await s.openFd('/lklmnt/a/f'), 5);
    await assert.rejects(s.readdir('/lklmnt/a'));
    const n = calls.length;
    await s.close();
    assert.equal(calls.length, n, 'no engine call on a dead engine');
});

test(
    'close() while a readdir is wedged resolves after the watchdog, no diskClose',
    { timeout: 3000 },
    async () => {
        const { bridge, calls } = fakeBridge({ diskOpen: async () => 0 });
        const s = new NativeSession(bridge, { opTimeoutMs: 100 });
        await s.attachPath('/img');
        const r = s.readdir('/a');
        r.catch(() => {});
        await tick();
        await s.close();
        await assert.rejects(r, /readdir timed out/);
        assert.ok(!calls.includes('diskClose'));
    },
);

test('a host fatal during close() releases the wait', { timeout: 3000 }, async () => {
    let push;
    const { bridge, calls } = fakeBridge({
        diskOpen: async () => 0,
        onFatal: (cb) => ((push = cb), () => {}),
    });
    const s = new NativeSession(bridge, { opTimeoutMs: 0 });
    await s.attachPath('/img');
    const r = s.readdir('/a');
    r.catch(() => {});
    await tick();
    const c = s.close();
    await tick();
    push('dead');
    await c;
    assert.ok(!calls.includes('diskClose'));
});

test('an attach queued behind a fatal never reaches the engine', { timeout: 3000 }, async () => {
    const { bridge, calls } = fakeBridge({});
    const s = new NativeSession(bridge, { opTimeoutMs: 50 });
    const r = s.readdir('/a');
    const at = s.attachPath('/img');
    await assert.rejects(r, /readdir timed out/);
    await assert.rejects(at, /readdir timed out/);
    assert.ok(!calls.includes('diskOpen'));
});

test(
    'a healthy close sends fileClose per fd, then diskClose, after in-flight ops',
    { timeout: 3000 },
    async () => {
        let n = 0;
        let release;
        const { bridge, calls } = fakeBridge({
            diskOpen: async () => 0,
            fileOpen: async () => ++n + 10,
            fileClose: async () => 0,
            diskClose: async () => 0,
            readdirJson: () => new Promise((r) => (release = () => r('[]'))),
        });
        const s = new NativeSession(bridge, { opTimeoutMs: 0 });
        await s.attachPath('/img');
        await s.openFd('/a');
        await s.openFd('/b');
        const r = s.readdir('/');
        await tick();
        const c = s.close();
        await tick();
        assert.ok(!calls.includes('fileClose'), 'waits for the in-flight op');
        release();
        await c;
        await r;
        assert.deepEqual(calls.slice(calls.indexOf('readdirJson') + 1), [
            'fileClose',
            'fileClose',
            'diskClose',
        ]);
    },
);

test(
    'after a host fatal close() skips diskClose but still stops the proxy',
    { timeout: 3000 },
    async () => {
        let push;
        const { bridge, calls } = fakeBridge({
            startProxy: async () => ({ proxyUrl: 'http://x/', id: 'p1' }),
            diskOpen: async () => 0,
            stopProxy: async () => {},
            onFatal: (cb) => ((push = cb), () => {}),
        });
        const s = new NativeSession(bridge);
        await s.attachUrl('http://up/');
        push('dead');
        await s.close();
        assert.ok(!calls.includes('diskClose'));
        assert.ok(calls.includes('stopProxy'));
    },
);

test('prewarmNative forwards opTimeoutMs', { timeout: 3000 }, async () => {
    const { bridge } = fakeBridge({ available: async () => true, init: async () => 0 });
    globalThis.anyfsNative = bridge;
    try {
        const s = await prewarmNative({ opTimeoutMs: 50 });
        const t0 = Date.now();
        await assert.rejects(s.readdir('/a'), /timed out after 0\.05s/);
        assert.ok(Date.now() - t0 < 1000);
    } finally {
        delete globalThis.anyfsNative;
    }
});
