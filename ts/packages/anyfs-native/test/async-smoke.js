/**
 * AsyncWorker verification gate — Task 17 of the open-image refactor plan.
 *
 * Tests that kernelInit (converted to AsyncWorker) works correctly and that
 * the mutex does not deadlock when mixing async init + synchronous ops.
 *
 * Usage:
 *   IMAGE=/path/to/disk.img node ts/packages/anyfs-native/test/async-smoke.js
 */
const path = require('path');
const addon = require('../build/Release/anyfs_native.node');

const IMG =
    process.env.IMAGE ||
    path.resolve(__dirname, '../../../../ts/examples/vite-demo/public/disks/multi.img');

async function main() {
    console.log('1. kernelInit (async)...');
    const rc = await addon.kernelInit(256, 0);
    console.log('   rc =', rc);
    if (rc !== 0) throw new Error(`init failed: ${rc}`);

    // The rest of the ops are still sync for now; test that the mutex
    // doesn't deadlock by running init (async) then sessionOpen (sync)
    console.log('2. sessionOpen (sync, should not deadlock)...');
    const h = addon.sessionOpen(IMG, 1);
    console.log('   handle =', h);
    if (h < 0) throw new Error(`sessionOpen failed: ${h}`);

    console.log('3. sessionListJson (sync)...');
    const parts = JSON.parse(addon.sessionListJson(h));
    console.log('   parts =', parts.length);

    addon.sessionClose(h);
    addon.kernelHalt();
    console.log('PASS: async init + sync ops, no deadlock or panic');
}

main().catch((e) => {
    console.error('FAIL:', e.message);
    process.exit(1);
});
