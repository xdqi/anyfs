/*
 * Quick test: open Debian netinst ISO and try entering partition 1.
 * Goal: reproduce the crash with proper Linux kernel messages.
 */
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const path = require('path');

const addon = require('/home/kosaka/anyfs-reader/ts/packages/anyfs-native/build/Release/anyfs_native.node');

const ISO = '/home/kosaka/debian-13.5.0-amd64-netinst.iso';

console.log('=== Debian ISO crash repro ===');
console.log('Addon exports:', Object.keys(addon));

// 1. Init kernel
console.log('\n1. Init kernel (mem=256, loglevel=5)...');
const rc = addon.init(256, 5);
console.log(`   init rc=${rc}`);

// 2. Open disk
console.log(`\n2. diskOpen(${ISO}, 1)`);
const handle = addon.diskOpen(ISO, 1);
console.log(`   handle=${handle}`);
if (handle < 0) {
    console.log('   FAILED');
    process.exit(1);
}

// 3. List
console.log('\n3. diskListJson...');
const list = JSON.parse(addon.diskListJson(handle));
console.log(`   ${list.length} entries`);
for (const e of list) {
    console.log(`   index=${e.index} slot_id=${e.slot_id} kind=${e.kind} fstype=${e.fstype || '?'} name="${e.label || ''}" size=${e.size}`);
}

// 4. Try to enter partition 1 (first entry that's not iso9660 or whichever is first partition)
console.log('\n4. Testing each entry...');
for (const e of list) {
    console.log(`\n--- Trying to enter [${e.index}] kind=${e.kind} type=${e.fstype || '?'} ---`);
    try {
        const lp = addon.diskEnter(handle, e.index, 1);
        console.log(`   diskEnter -> "${lp}"`);
        if (lp && lp.length > 0) {
            // Try to readdir
            console.log(`   readdirJson("${lp}", "")...`);
            try {
                const rd = JSON.parse(addon.readdirJson(lp, ''));
                console.log(`   readdir OK: ${rd.length} entries`);
                for (const f of rd.slice(0, 10)) {
                    console.log(`     ${f.name} (${f.kind})`);
                }
                if (rd.length > 10) console.log(`     ... +${rd.length - 10} more`);
            } catch (err) {
                console.log(`   readdir FAILED: ${err.message}`);
            }
        }
    } catch (err) {
        console.log(`   diskEnter FAILED: ${err.message}`);
    }
}

console.log('\n=== DONE ===');
addon.kernelHalt();
