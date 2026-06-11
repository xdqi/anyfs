import { test, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { createSession } from '../dist/index.js';

afterEach(() => {
    delete globalThis.anyfsNative;
});

// getAnyfsNative() (src/native-session.ts) detects the bridge by checking
// `typeof g.init === 'function'` — the fake must provide init() or the
// electron path silently falls back to wasm.
const fakeBridge = { init() {}, sessionOpen() {}, kernelInit() {} };

test('web → wasm, blob+url only, no bridge', () => {
    const r = createSession('web');
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.allowedKinds, new Set(['blob', 'url']));
    assert.equal(r.nativeBridge, undefined);
    assert.equal(typeof r.wasmCaps, 'object');
});

test('node → node-wasm, path only', () => {
    const r = createSession('node');
    assert.equal(r.backend, 'node-wasm');
    assert.deepEqual(r.allowedKinds, new Set(['path']));
});

test('electron + bridge → native, path+url', () => {
    globalThis.anyfsNative = fakeBridge;
    const r = createSession('electron');
    assert.equal(r.backend, 'native');
    assert.equal(r.nativeBridge, fakeBridge);
    assert.deepEqual(r.allowedKinds, new Set(['path', 'url']));
});

test('electron + bridge + disableNative → wasm fallback', () => {
    globalThis.anyfsNative = fakeBridge;
    const r = createSession('electron', { disableNative: true });
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.allowedKinds, new Set(['blob', 'url']));
});

test('electron without bridge → wasm fallback', () => {
    const r = createSession('electron');
    assert.equal(r.backend, 'wasm');
    assert.deepEqual(r.allowedKinds, new Set(['blob', 'url']));
});

test('electron wasm + pathLoopbackUrl cap → path allowed', () => {
    const r = createSession('electron', {
        disableNative: true,
        electronWasmCaps: { pathLoopbackUrl: 'http://127.0.0.1:9999/d0' },
    });
    assert.equal(r.backend, 'wasm');
    assert.ok(r.allowedKinds.has('path'));
    assert.equal(r.wasmCaps.pathLoopbackUrl, 'http://127.0.0.1:9999/d0');
});

test('electron wasm caps forwarded verbatim (urlProxyPrefix)', () => {
    const r = createSession('electron', {
        disableNative: true,
        electronWasmCaps: { urlProxyPrefix: 'anyfs-url://proxy/?u=' },
    });
    assert.equal(r.wasmCaps.urlProxyPrefix, 'anyfs-url://proxy/?u=');
    assert.ok(!r.allowedKinds.has('path')); // no loopback cap -> no path
});
