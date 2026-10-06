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
    fail(id, error) {
        this.dispatchEvent(new MessageEvent('message', { data: { id, ok: false, error } }));
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
        const s = new WasmSession(w, { opTimeoutMs: 200 });
        let fatal = null;
        s.onFatal((e) => (fatal = e));
        const a = s.readdir('/a');
        const b = s.stat('/b');
        await new Promise((r) => setImmediate(r));
        assert.equal(w.posted.length, 1);
        await new Promise((r) => setTimeout(r, 150));
        w.reply(w.posted[0].id, [{ name: 'a', ino: 1, kind: 'file' }]);
        while (w.posted.length < 2) await new Promise((r) => setImmediate(r));
        await new Promise((r) => setTimeout(r, 120));
        w.reply(w.posted[1].id, { size: 7 });
        assert.deepEqual(await a, [{ name: 'a', ino: 1, kind: 'file' }]);
        assert.deepEqual(await b, { size: 7 });
        assert.equal(fatal, null);
    },
);

const tick = () => new Promise((r) => setImmediate(r));

test('close() with a read in flight terminates at once', { timeout: 2000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 0 });
    let fatal = null;
    s.onFatal((e) => (fatal = e));
    const o = s.openFd('/f');
    await tick();
    w.reply(w.posted[0].id, 3);
    assert.equal(await o, 3);
    const r = s.readFd(3, 0, 10);
    r.catch(() => {});
    await tick();
    await s.close();
    assert.equal(w.terminated, true);
    assert.equal(fatal, null);
    await assert.rejects(r);
});

test('a rejected op does not block the next one', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 500 });
    const a = s.stat('/a');
    const b = s.stat('/b');
    await tick();
    assert.equal(w.posted.length, 1);
    w.fail(w.posted[0].id, 'EIO');
    await assert.rejects(a, /EIO/);
    await tick();
    assert.equal(w.posted.length, 2);
    w.reply(w.posted[1].id, { size: 1 });
    assert.deepEqual(await b, { size: 1 });
});

test('a worker error rejects queued ops and posts nothing more', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 500 });
    const a = s.stat('/a');
    const b = s.stat('/b');
    const c = s.stat('/c');
    await tick();
    const ev = new Event('error');
    ev.message = 'boom';
    w.dispatchEvent(ev);
    await assert.rejects(a, /boom/);
    await assert.rejects(b, /boom/);
    await assert.rejects(c, /boom/);
    assert.equal(w.posted.length, 1);
});

test('attach and ops reach the worker in call order', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 500 });
    const k = s.readKernelFile('/proc/x');
    const at = s.attachBlob(new Blob([new Uint8Array(4)]));
    await tick();
    assert.deepEqual(
        w.posted.map((m) => m.op),
        ['readKernelFile'],
    );
    w.reply(w.posted[0].id, 'x');
    await k;
    await tick();
    assert.deepEqual(
        w.posted.map((m) => m.op),
        ['readKernelFile', 'attach'],
    );
    w.reply(w.posted[1].id, null);
    await at;
});

test('an attach queued behind a watchdog fatal is never posted', { timeout: 5000 }, async () => {
    const w = new FakeWorker();
    const s = new WasmSession(w, { opTimeoutMs: 100 });
    const k = s.readKernelFile('/proc/x');
    k.catch(() => {});
    const at = s.attachBlob(new Blob([new Uint8Array(4)]));
    await assert.rejects(k, /readKernelFile timed out/);
    await assert.rejects(at, /readKernelFile timed out/);
    assert.deepEqual(
        w.posted.map((m) => m.op),
        ['readKernelFile'],
    );
});
