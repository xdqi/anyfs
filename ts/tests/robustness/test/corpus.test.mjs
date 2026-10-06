import { before, test } from 'node:test';
import assert from 'node:assert/strict';
import { crc32c, crc32cRaw } from '../corpus/checksum.mjs';
import { randInt, rng } from '../corpus/rng.mjs';
import { join } from 'node:path';
import { BASES, TOOLS, buildAllBases } from '../corpus/bases.mjs';
import { fixBtrfsSbCsum, fixExt4SbCsum, fixGptCrcs, fixXfsSbCrc } from '../corpus/mutations.mjs';
import { requireTools } from '../corpus/tools.mjs';
import { SCRATCH_DIR } from '../lib/paths.mjs';

test('crc32c matches the standard check value', () => {
    assert.equal(crc32c(Buffer.from('123456789')), 0xe3069283);
    assert.equal(crc32cRaw(0xffffffff, Buffer.from('123456789')), ~0xe3069283 >>> 0);
});

test('rng is deterministic per seed', () => {
    const a = rng(1);
    const b = rng(1);
    const c = rng(2);
    const sa = [a(), a(), a()];
    assert.deepEqual([b(), b(), b()], sa);
    assert.notDeepEqual([c(), c(), c()], sa);
    const next = rng(7);
    for (let i = 0; i < 1000; i++) {
        const n = randInt(next, 10);
        assert.ok(Number.isInteger(n) && n >= 0 && n < 10);
    }
});

// Built once for every test below: needs the image tools (Task 0).
let bases;
before(() => {
    requireTools(TOOLS);
    bases = buildAllBases(join(SCRATCH_DIR, 'bases'));
});

test('there are 16 bases', () => {
    assert.equal(Object.keys(BASES).length, 16);
});

test('checksum fix-ups reproduce the checksums mkfs wrote', () => {
    for (const [name, fix] of [
        ['ext4', fixExt4SbCsum],
        ['btrfs', fixBtrfsSbCsum],
        ['xfs', fixXfsSbCrc],
        ['gpt', fixGptCrcs],
    ]) {
        const b = Buffer.from(bases.bufs[name]);
        fix(b);
        assert.ok(b.equals(bases.bufs[name]), `${name}: recomputed checksum differs`);
    }
});

test('layouts point at the structures they name', () => {
    const { bufs, layouts } = bases;

    const e = layouts.ext4;
    assert.notEqual(e.rootInode, e.docsInode);
    assert.equal(bufs.ext4.readUInt16LE(e.rootInode) & 0xf000, 0x4000, 'root inode is a dir');
    assert.equal(bufs.ext4.readUInt16LE(e.docsInode) & 0xf000, 0x4000, 'docs inode is a dir');
    assert.equal(bufs.ext4panic.readUInt16LE(1024 + 60), 3, 'ext4panic sb says errors=panic');

    const v = layouts.vfat;
    assert.equal(bufs.vfat.readUInt16LE(v.fatOffset), 0xfff8, 'FAT16 media entry');
    const rootDir = bufs.vfat.subarray(v.rootDirOffset, v.rootDirOffset + v.rootDirBytes);
    assert.ok(rootDir.includes('HELLO   TXT'), 'root dir holds the 8.3 entry of hello.txt');

    const bt = layouts.btrfs;
    assert.ok(bt.treeBlocks['3']?.length > 0, 'chunk tree blocks found');
    assert.ok(bt.treeBlocks['5']?.length > 0, 'fs tree blocks found');

    assert.equal(bufs.xfs.readUInt16BE(layouts.xfs.rootInode), 0x494e, 'xfs root inode magic');

    const iso = layouts.iso9660;
    assert.equal(iso.blockSize, 2048);
    assert.ok(bufs.iso9660[iso.rootDirOffset] >= 34, 'root dir starts with a record');
});
