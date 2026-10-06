import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { before, describe, test } from 'node:test';
import { BASES, TOOLS, buildAllBases } from '../corpus/bases.mjs';
import { crc32c, crc32cRaw } from '../corpus/checksum.mjs';
import { probeLayout } from '../corpus/layout.mjs';
import {
    fixBtrfsSbCsum,
    fixExt4SbCsum,
    fixGptCrcs,
    fixXfsSbCrc,
    flip,
    mutationCases,
    truncate,
} from '../corpus/mutations.mjs';
import { randInt, rng } from '../corpus/rng.mjs';
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

test('62 mutations over the bases, 78 cases in all, unique names', () => {
    const m = mutationCases();
    assert.equal(m.length, 62);
    const names = [...Object.keys(BASES).map((b) => `${b}-base`), ...m.map((x) => x.name)];
    assert.equal(names.length, 78);
    assert.equal(new Set(names).size, names.length);
    for (const x of m) assert.ok(BASES[x.base], `${x.name}: unknown base ${x.base}`);
});

test('there are 16 bases', () => {
    assert.equal(Object.keys(BASES).length, 16);
});

test('flip and truncate', () => {
    const z = Buffer.alloc(1 << 20);
    const f = flip(z, 1, 1e-3);
    let changed = 0;
    for (const x of f) if (x) changed++;
    assert.ok(changed > 900 && changed <= 1049, `changed ${changed}`);
    assert.equal(truncate(Buffer.alloc(10_000), 25).length, 2048);
});

// These need the image tools (mke2fs, mkfs.*, xorriso, ...); a missing tool
// fails only this suite, not the pure tests above.
describe('base images', () => {
    let bases;
    before(() => {
        requireTools(TOOLS);
        bases = buildAllBases(join(SCRATCH_DIR, 'bases'));
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
        for (const n of ['ext4', 'ext4panic']) {
            assert.ok(bufs[n].readUInt32LE(1024 + 100) & 0x400, `${n} has metadata_csum`);
        }

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
        assert.ok(iso.docsDirOffset > 0, 'docs extent found');
        assert.ok(
            bufs.iso9660[iso.docsDirOffset] >= 34,
            'docs extent starts with a directory record',
        );
        assert.equal(
            bufs.iso9660[iso.docsDirOffset + 25] & 2,
            2,
            'docs extent is flagged a directory',
        );
    });

    const sha = (b) => createHash('sha256').update(b).digest('hex');

    test('mutations are pure, deterministic and actually change the image', () => {
        for (const m of mutationCases()) {
            const base = bases.bufs[m.base];
            const before = sha(base);
            const a = m.apply(base, bases.layouts[m.base]);
            const b = m.apply(base, bases.layouts[m.base]);
            assert.equal(sha(base), before, `${m.name} modified its base`);
            assert.ok(a.equals(b), `${m.name} is not deterministic`);
            assert.ok(!a.equals(base), `${m.name} left the image unchanged`);
        }
    });

    const MAGIC = {
        ext4: (b) => b.readUInt16LE(1024 + 56) === 0xef53,
        ext4panic: (b) => b.readUInt16LE(1024 + 56) === 0xef53,
        btrfs: (b) => b.toString('latin1', 0x10040, 0x10048) === '_BHRfS_M',
        xfs: (b) => b.toString('latin1', 0, 4) === 'XFSB',
        vfat: (b) => b.readUInt16LE(510) === 0xaa55,
        iso9660: (b) => b.toString('latin1', 16 * 2048 + 1, 16 * 2048 + 6) === 'CD001',
    };
    const FIXUP = {
        ext4: fixExt4SbCsum,
        ext4panic: fixExt4SbCsum,
        btrfs: fixBtrfsSbCsum,
        xfs: fixXfsSbCrc,
    };

    test('mutations keep what they promise to keep', () => {
        // Build only the images inspected here, one at a time.
        const INSPECTED =
            /-sb-|^iso9660-pvd-|^gpt-|^btrfs-zero-(chunk|fs)-tree$|^mbrext-ext-loop$|^qcow2-l2-entry-at-header$/;
        const byName = {};
        for (const m of mutationCases().filter((x) => INSPECTED.test(x.name))) {
            byName[m.name] = m.apply(bases.bufs[m.base], bases.layouts[m.base]);
        }
        for (const m of mutationCases().filter((x) => x.name in byName)) {
            const out = byName[m.name];
            if (/-sb-|^iso9660-pvd-/.test(m.name)) {
                assert.ok(MAGIC[m.base](out), `${m.name}: magic was not kept`);
                const fix = FIXUP[m.base];
                if (fix) {
                    const again = Buffer.from(out);
                    fix(again);
                    assert.ok(again.equals(out), `${m.name}: checksum is not valid`);
                }
            }
            if (m.name.startsWith('gpt-')) {
                const again = Buffer.from(out);
                fixGptCrcs(again);
                assert.ok(again.equals(out), `${m.name}: GPT CRCs do not verify`);
            }
        }
        const find = (name) => byName[name];
        for (const [name, owner] of [
            ['btrfs-zero-chunk-tree', '3'],
            ['btrfs-zero-fs-tree', '5'],
        ]) {
            const l = probeLayout('btrfs', null, find(name));
            assert.equal(
                l.treeBlocks[owner]?.length ?? 0,
                0,
                `${name}: owner ${owner} blocks remain`,
            );
        }
        const q = find('qcow2-l2-entry-at-header');
        const l2 = Number(q.readBigUInt64BE(Number(q.readBigUInt64BE(40))) & 0x00fffffffffffe00n);
        assert.equal(q.readBigUInt64BE(l2 + 8), 0x8000000000000000n, 'L2[1] is COPIED, offset 0');
        const loop = find('mbrext-ext-loop');
        const e = loop.readUInt32LE(446 + 16 + 8) * 512 + 446 + 16;
        assert.equal(loop.readUInt32LE(e + 8), 0, 'EBR link start is 0');
        assert.equal(loop[e + 4], 0x05, 'EBR link type is extended');
    });
});
