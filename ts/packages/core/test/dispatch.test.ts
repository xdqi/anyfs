import { strictEqual, deepStrictEqual } from 'node:assert';
import { createSession } from '../src/dispatch.js';

let passed = 0;
let failed = 0;

function test(name: string, fn: () => void) {
    try {
        fn();
        passed++;
        console.log(`  ✓ ${name}`);
    } catch (e) {
        failed++;
        console.error(`  ✗ ${name}: ${(e as Error).message}`);
    }
}

// web: wasm backend, blob + url only
test('web → wasm backend', () => {
    const r = createSession('web');
    strictEqual(r.backend, 'wasm');
});

test('web → allowed blob+url', () => {
    const r = createSession('web');
    deepStrictEqual(r.allowedKinds, new Set(['blob', 'url']));
});

test('web → no nativeBridge', () => {
    const r = createSession('web');
    strictEqual(r.nativeBridge, undefined);
});

test('web → wasmCaps is present (empty)', () => {
    const r = createSession('web');
    strictEqual(typeof r.wasmCaps, 'object');
});

// node: node-wasm backend, path only
test('node → node-wasm backend', () => {
    const r = createSession('node');
    strictEqual(r.backend, 'node-wasm');
});

test('node → allowed path only', () => {
    const r = createSession('node');
    deepStrictEqual(r.allowedKinds, new Set(['path']));
});

test('node → no nativeBridge', () => {
    const r = createSession('node');
    strictEqual(r.nativeBridge, undefined);
});

// electron: no bridge → wasm fallback
test('electron (no bridge) → wasm backend', () => {
    const r = createSession('electron');
    strictEqual(r.backend, 'wasm');
});

test('electron (no bridge) → allowed blob+url (no pathLoopbackUrl)', () => {
    const r = createSession('electron');
    deepStrictEqual(r.allowedKinds, new Set(['blob', 'url']));
});

// F8 contract: wasm-under-electron can take a blob source (local files stay
// {kind:'blob'} when native is disabled, mounted via WORKERFS like the web path).
test('electron (no bridge) → allowedKinds includes blob (F8)', () => {
    const r = createSession('electron');
    strictEqual(r.allowedKinds.has('blob'), true);
});

// electron with pathLoopbackUrl: add path to allowed
test('electron (wasm + pathLoopbackUrl) → allowed blob+url+path', () => {
    const r = createSession('electron', {
        electronWasmCaps: { pathLoopbackUrl: 'http://127.0.0.1:12345/token123' },
    });
    deepStrictEqual(r.allowedKinds, new Set(['blob', 'url', 'path']));
});

// electron with disableNative: force wasm even if bridge present
test('electron (disableNative) → wasm backend', () => {
    const r = createSession('electron', { disableNative: true });
    strictEqual(r.backend, 'wasm');
});

console.log(`\n${passed} passed, ${failed} failed`);
if (failed > 0) process.exit(1);
