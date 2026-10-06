import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bootNodeKernel, haltKernel } from '../dist/node.js';

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

// Boot state is process-global, so this lives in its own file.
test('halt lifecycle', { timeout: 10000 }, async () => {
    let calls = 0;
    let onAbort;
    const mk = () => {
        const M = fakeModule();
        M.autoAnswer = true;
        return M;
    };
    const factory = async (o) => {
        calls++;
        onAbort = o.onAbort;
        return mk();
    };

    const M1 = await bootNodeKernel('/d', factory);
    assert.equal(await bootNodeKernel('/d/', factory), M1, 'trailing slash reuses');
    assert.equal(calls, 1);
    await assert.rejects(bootNodeKernel('/e', factory), /already/);

    await haltKernel();
    const M2 = await bootNodeKernel('/e', factory);
    assert.notEqual(M2, M1);
    assert.equal(calls, 2);

    // abort: halt sends no KERNEL_HALT and allows a new boot
    onAbort('dead');
    const before = M2.submitted.length;
    await haltKernel();
    assert.equal(M2.submitted.length, before);
    const M3 = await bootNodeKernel('/f', factory);
    assert.notEqual(M3, M2);

    // failed boot: KERNEL_INIT answered with -1
    await haltKernel();
    const bad = async () => {
        const M = fakeModule();
        M.ccall = (_n, _r, _t, [req]) => {
            M.submitted.push(req);
            queueMicrotask(() => M.answer(-1));
            return 0;
        };
        return M;
    };
    await assert.rejects(bootNodeKernel('/g', bad), /kernel_init failed/);
    await haltKernel();
    const M4 = await bootNodeKernel('/h', factory);
    assert.notEqual(M4, M3);
});
