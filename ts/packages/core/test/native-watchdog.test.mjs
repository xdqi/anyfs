import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NativeSession } from '../dist/index.js';

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
    'close() after a watchdog fatal does not wait on the wedged engine',
    { timeout: 2000 },
    async () => {
        const { bridge, calls } = fakeBridge({ diskOpen: async () => 0, fileOpen: async () => 5 });
        const s = new NativeSession(bridge, { opTimeoutMs: 30 });
        await s.attachPath('/img');
        assert.equal(await s.openFd('/lklmnt/a/f'), 5);
        await assert.rejects(s.readdir('/lklmnt/a'));
        await s.close();
        assert.ok(!calls.includes('fileClose'), 'must not close fds on a dead engine');
        assert.ok(!calls.includes('diskClose'), 'must not close the disk on a dead engine');
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
