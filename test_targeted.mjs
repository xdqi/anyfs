/*
 * Test specific partition entries on the Debian ISO.
 */
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const addon = require('/home/kosaka/anyfs-reader/ts/packages/anyfs-native/build/Release/anyfs_native.node');

const ISO = '/home/kosaka/debian-13.5.0-amd64-netinst.iso';

console.log('=== Targeted partition test ===');

// 1. Init
console.log('1. init(256,7)...');
const rc0 = addon.init(256, 7);
console.log(`   rc=${rc0}`);

// 2. Open disk
console.log('2. diskOpen...');
const h = addon.diskOpen(ISO, 1);
console.log(`   handle=${h}`);

// 3. List
const list = JSON.parse(addon.diskListJson(h));
console.log('3. Partitions:');
for (const e of list) {
    console.log(`   index=${e.index} slot_id=${e.slot_id} kind=${e.kind} fstype=${e.fstype || '?'} label="${e.label || ''}" size=${e.size}`);
}

// 4. Try mountWhole first (mount the whole disk as iso9660)
console.log('\n4. mountWhole(handle, "iso9660", 1)...');
try {
    const mp = addon.mountWhole(h, 'iso9660', 1);
    console.log(`   mountWhole -> "${mp}"`);
    if (mp) {
        const rd = JSON.parse(addon.readdirJson(mp, ''));
        console.log(`   readdir: ${rd.length} entries`);
        for (const f of rd.slice(0, 5)) console.log(`     ${f.name} (${f.kind})`);
    }
} catch (err) {
    console.log(`   FAILED: ${err.message}`);
}

console.log('\n=== DONE ===');
addon.kernelHalt();
