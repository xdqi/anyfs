import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bootNodeKernel } from '../dist/node.js';

// Own file: the boot state is process-global.
test('an abort before the factory resolves fails the boot', { timeout: 3000 }, async () => {
    const M = {
        HEAP32: new Int32Array(1024),
        HEAPU8: new Uint8Array(4096),
        _malloc: () => 64,
        _free() {},
        ccall: () => 0, // the API thread never answers
    };
    const factory = async (opts) => {
        opts.onAbort('x');
        return M;
    };
    await assert.rejects(bootNodeKernel('/x', factory), /wasm module aborted: x/);
});
