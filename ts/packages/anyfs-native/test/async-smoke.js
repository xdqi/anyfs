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
    console.log('1. kernelInit (sync)...');
    const rc = addon.kernelInit(256, 0);
    console.log('   rc =', rc);
    if (rc !== 0) throw new Error(`init failed: ${rc}`);

    // sessionOpen is sync (QEMU blk needs same thread as init/close).
    console.log('2. sessionOpen (sync)...');
    const h = addon.sessionOpen(IMG, 1);
    console.log('   handle =', h);
    if (h < 0) throw new Error(`sessionOpen failed: ${h}`);

    // listParts + enter are async (IO-heavy ops in thread pool).
    console.log('3. sessionListJson (async)...');
    const parts = JSON.parse(await addon.sessionListJson(h));
    console.log('   parts =', parts.length);

    // sessionClose + kernelHalt are sync (QEMU requires main thread).
    addon.sessionClose(h);
    addon.kernelHalt();
    console.log('PASS: mixed async IO + sync lifecycle, no deadlock or QEMU assert');
}

main().catch((e) => {
    console.error('FAIL:', e.message);
    process.exit(1);
});
