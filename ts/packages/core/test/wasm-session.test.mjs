import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WasmSession } from '../dist/index.js';

/** Stands in for the module Worker: records posts and only answers when told. */
class FakeWorker extends EventTarget {
    constructor() {
        super();
        this.posted = [];
        this.terminated = false;
    }
    postMessage(m) {
        this.posted.push(m);
    }
    terminate() {
        this.terminated = true;
    }
    reply(id, result) {
        this.dispatchEvent(new MessageEvent('message', { data: { id, ok: true, result } }));
    }
}

test(
    'a wedged readdir rejects and fires onFatal within opTimeoutMs',
    { timeout: 5000 },
    async () => {
        const w = new FakeWorker();
        const s = new WasmSession(w, { opTimeoutMs: 50 });
        const fatals = [];
        s.onFatal((e) => fatals.push(e.message));
        await assert.rejects(
            s.readdir('/lklmnt/x'),
            /readdir timed out after 0\.05s — the engine is wedged/,
        );
        assert.equal(fatals.length, 1);
        assert.equal(w.posted.at(-1).op, 'readdir');
        // Further ops reject at once and never reach the worker.
        const n = w.posted.length;
        await assert.rejects(s.stat('/lklmnt/x'), /readdir timed out/);
        assert.equal(w.posted.length, n);
    },
);

test('a reply inside the window resolves normally', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 500 });
    const p = s.readdir('/');
    await new Promise((r) => setImmediate(r));
    w.reply(w.posted[0].id, [{ name: 'a', ino: 1, kind: 'file' }]);
    assert.deepEqual(await p, [{ name: 'a', ino: 1, kind: 'file' }]);
});

test('attach is not under the op watchdog', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 20 });
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    const r = await Promise.race([
        s.attachBlob(new Blob([new Uint8Array(4)])).then(
            () => 'settled',
            () => 'settled',
        ),
        new Promise((res) => setTimeout(() => res('pending'), 100)),
    ]);
    assert.equal(r, 'pending');
    assert.equal(fatal, null);
});

test('close() after a watchdog fatal terminates the worker', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 20 });
    await assert.rejects(s.readdir('/'));
    await s.close();
    assert.equal(w.terminated, true);
});

test(
    'the timer starts when an op reaches the worker, not while it queues',
    { timeout: 5000 },
    async () => {
        const w = new FakeWorker();
        const s = new WasmSession(w, { opTimeoutMs: 100 });
        let fatal = null;
        s.onFatal((e) => (fatal = e));
        const a = s.readdir('/a');
        const b = s.stat('/b');
        await new Promise((r) => setImmediate(r));
        assert.equal(w.posted.length, 1);
        await new Promise((r) => setTimeout(r, 80));
        w.reply(w.posted[0].id, [{ name: 'a', ino: 1, kind: 'file' }]);
        while (w.posted.length < 2) await new Promise((r) => setImmediate(r));
        await new Promise((r) => setTimeout(r, 60));
        w.reply(w.posted[1].id, { size: 7 });
        assert.deepEqual(await a, [{ name: 'a', ino: 1, kind: 'file' }]);
        assert.deepEqual(await b, { size: 7 });
        assert.equal(fatal, null);
    },
);
