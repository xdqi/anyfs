import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NodeWasmSession } from '../dist/index.js';
import { bootNodeKernel } from '../dist/node.js';

/** Just enough of the emscripten module for WasmApi: a heap, a bump
 *  allocator, and an "API thread" that answers only when told to. */
function fakeModule() {
    const heap = new ArrayBuffer(1 << 20);
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const M = {
        HEAPU8: new Uint8Array(heap),
        HEAP32: new Int32Array(heap),
        HEAPU32: new Uint32Array(heap),
        top: 1024,
        submitted: [],
        autoAnswer: false,
        _malloc(n) {
            const p = M.top;
            M.top += (n + 7) & ~7;
            return p;
        },
        _free() {},
        ccall(name, _ret, _types, [req]) {
            assert.equal(name, 'anyfs_ts_api_submit');
            M.submitted.push(req);
            if (M.autoAnswer) queueMicrotask(() => M.answer(0));
            return 0;
        },
        /** The API thread finishes the oldest request with return value `ret`. */
        answer(ret) {
            const w = M.submitted.shift() >> 2;
            M.HEAP32[w + 2] = ret;
            M.anyfsApiDone(M.HEAP32[w + 1]);
        },
        lengthBytesUTF8: (s) => enc.encode(s).length,
        stringToUTF8(s, p, max) {
            const b = enc.encode(s).subarray(0, max - 1);
            M.HEAPU8.set(b, p);
            M.HEAPU8[p + b.length] = 0;
        },
        UTF8ToString(p, max = Infinity) {
            let e = p;
            while (e - p < max && M.HEAPU8[e] !== 0) e++;
            return dec.decode(M.HEAPU8.subarray(p, e));
        },
    };
    return M;
}

test('a wedged op rejects and fires onFatal within opTimeoutMs', { timeout: 5000 }, async () => {
    const s = new NodeWasmSession(fakeModule(), { opTimeoutMs: 50 });
    const fatals = [];
    s.onFatal((e) => fatals.push(e.message));
    await assert.rejects(
        s.readdir('/work'),
        /readdir timed out after 0\.05s — the engine is wedged/,
    );
    assert.equal(fatals.length, 1);
    await s.close(); // must not wait on the wedged API thread
});

test('readOnly opens the image with ANYFS_SESSION_READONLY', { timeout: 5000 }, async () => {
    const M = fakeModule();
    const s = new NodeWasmSession(M, { readOnly: true });
    const p = s.attachPath('/work/disk.img');
    const w = M.submitted[0] >> 2;
    assert.equal(M.HEAP32[w], 3); // ApiOp.SESSION_OPEN
    assert.equal(M.HEAP32[w + 4], 1); // flags
    M.answer(0);
    await p;
});

test('a module abort rejects pending ops and fires onFatal', { timeout: 5000 }, async () => {
    const M = fakeModule();
    let onAbort;
    const factory = async (opts) => {
        onAbort = opts.onAbort;
        return M;
    };
    M.autoAnswer = true; // KERNEL_INIT succeeds at once
    assert.equal(await bootNodeKernel('/nonexistent', factory), M);
    M.autoAnswer = false; // from here on, ops wedge until answered
    const s = new NodeWasmSession(M, { opTimeoutMs: 0 });
    const fatals = [];
    s.onFatal((e) => fatals.push([e.name, e.message]));
    const pending = s.readdir('/work');
    onAbort('native code called abort()');
    await assert.rejects(pending, /wasm module aborted: native code called abort\(\)/);
    assert.deepEqual(fatals, [
        ['EngineFatalError', 'wasm module aborted: native code called abort()'],
    ]);
    await assert.rejects(s.stat('/work'), /wasm module aborted/);
});
