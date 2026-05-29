/*
 * Test via HTTP proxy to match Windows behavior.
 */
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
import { Worker } from 'node:worker_threads';

const addon = require('/home/kosaka/anyfs-reader/ts/packages/anyfs-native/build/Release/anyfs_native.node');

console.log('=== HTTP proxy test ===');

// 1. Init
console.log('1. init(256,7)...');
const rc0 = addon.init(256, 7);
console.log(`   rc=${rc0}`);

// 2. Open via local file (same as before, works)
console.log('2. diskOpen(file)...');
const h = addon.diskOpen('/home/kosaka/debian-13.5.0-amd64-netinst.iso', 1);
console.log(`   handle=${h}`);

// 3. List
const list = JSON.parse(addon.diskListJson(h));
console.log('3. Partition entries:');
for (const e of list) {
    console.log(`   index=${e.index} kind=${e.kind} fstype=${e.fstype || '?'} size=${e.size}`);
}

// 4. Mount whole as iso9660
console.log('\n4. mountWhole(iso9660)...');
const mp = addon.mountWhole(h, 'iso9660', 1);
console.log(`   mount path: "${mp}"`);
const rd = JSON.parse(addon.readdirJson(mp, ''));
console.log(`   readdir: ${rd.length} entries`);
for (const f of rd.slice(0, 5)) console.log(`     ${f.name}`);

// 5. Enter NESTED entry and check children
console.log('\n5. Entering NESTED entry (index=1)...');
const lp = addon.diskEnter(h, 1, 1);
console.log(`   diskEnter -> "${lp || '(empty)'}"`);

// 6. Check for children after entering NESTED
if (lp === '') {
    console.log('   No mount path - checking for child partitions...');
    const list2 = JSON.parse(addon.diskListJson(h));
    console.log(`   After enter: ${list2.length} entries`);
    for (const e of list2) {
        console.log(`     index=${e.index} slot_id=${e.slot_id} parent=${e.parent} kind=${e.kind} fstype=${e.fstype || '?'} size=${e.size}`);
    }
}

console.log('\n=== DONE ===');
addon.kernelHalt();
