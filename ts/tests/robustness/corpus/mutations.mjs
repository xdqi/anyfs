/**
 * Deterministic mutations of the base images. Each is a pure function of
 * (base bytes, layout[, seed]) and returns a new buffer; the base is never
 * modified.
 */
import { crc32 } from 'node:zlib';
import { crc32c, crc32cRaw } from './checksum.mjs';
import { randInt, rng } from './rng.mjs';

// ── Checksum fix-ups ──
// Re-checksum after editing a field, so the driver parses the extreme value
// itself instead of stopping at the checksum check.

/** ext4: raw crc32c of superblock bytes 0..1019, only with metadata_csum. */
export function fixExt4SbCsum(img) {
    const sb = img.subarray(1024, 2048);
    if (!(sb.readUInt32LE(100) & 0x400)) return;
    sb.writeUInt32LE(crc32cRaw(0xffffffff, sb.subarray(0, 1020)), 1020);
}

/** btrfs: standard crc32c of superblock [0x20, 0x1000), stored at 0. */
export function fixBtrfsSbCsum(img) {
    const sb = img.subarray(0x10000, 0x11000);
    sb.writeUInt32LE(crc32c(sb.subarray(0x20)), 0);
}

/** xfs: standard crc32c of the superblock sector, crc field zeroed. */
export function fixXfsSbCrc(img) {
    const sb = img.subarray(0, img.readUInt16BE(102));
    sb.writeUInt32LE(0, 224);
    sb.writeUInt32LE(crc32c(sb), 224);
}

/** GPT: CRC-32 of the entry array, then of the primary header. */
export function fixGptCrcs(img) {
    const hdr = img.subarray(512, 1024);
    const at = Number(hdr.readBigUInt64LE(72)) * 512;
    const bytes = hdr.readUInt32LE(80) * hdr.readUInt32LE(84);
    hdr.writeUInt32LE(crc32(img.subarray(at, at + bytes)), 88);
    hdr.writeUInt32LE(0, 16);
    hdr.writeUInt32LE(crc32(hdr.subarray(0, hdr.readUInt32LE(12))), 16);
}

const MiB = 1 << 20;

/** Edit a copy, then optionally re-checksum it. */
const edit = (fn, fix) => (buf, layout) => {
    const out = Buffer.from(buf);
    fn(out, layout);
    fix?.(out);
    return out;
};

/** Zero the [offset, length] ranges `pick(layout)` names, in a copy. */
const zero = (pick) => (buf, layout) => {
    const out = Buffer.from(buf);
    for (const [off, len] of pick(layout)) out.fill(0, off, off + len);
    return out;
};

/** Random byte corruption over the first 4 MiB: XOR `density * span` bytes. */
export function flip(buf, seed, density) {
    const out = Buffer.from(buf);
    const span = Math.min(out.length, 4 * MiB);
    const next = rng(seed);
    const n = Math.max(1, Math.round(span * density));
    for (let i = 0; i < n; i++) out[randInt(next, span)] ^= 1 + randInt(next, 255);
    return out;
}

/** The first `pct` % of the image, rounded down to a sector. */
export const truncate = (buf, pct) =>
    Buffer.from(buf.subarray(0, Math.floor((buf.length * pct) / 100 / 512) * 512));

/** 1. Superblock fields set to extreme values; magic kept, checksum fixed. */
const SB_EDITS = {
    ext4: [
        ['sb-blocks-count', (b) => b.writeUInt32LE(0xffffffff, 1024 + 4)],
        ['sb-inodes-per-group', (b) => b.writeUInt32LE(0xffffffff, 1024 + 40)],
        ['sb-log-block-size', (b) => b.writeUInt32LE(31, 1024 + 24)],
    ],
    vfat: [
        ['sb-sectors-per-cluster', (b) => b.writeUInt8(0, 13)],
        ['sb-reserved-sectors', (b) => b.writeUInt16LE(0xffff, 14)],
        [
            'sb-total-sectors',
            (b) => {
                b.writeUInt16LE(0, 19);
                b.writeUInt32LE(0xffffffff, 32);
            },
        ],
    ],
    btrfs: [
        ['sb-nodesize', (b) => b.writeUInt32LE(1 << 20, 0x10000 + 0x94)],
        ['sb-root', (b) => b.writeBigUInt64LE(0x7fffffff0000n, 0x10000 + 0x50)],
    ],
    xfs: [
        ['sb-agcount', (b) => b.writeUInt32BE(0xffffffff, 88)],
        ['sb-rootino', (b) => b.writeBigUInt64BE(0xfffffffff0n, 56)],
    ],
    iso9660: [
        [
            'pvd-block-size',
            (b, l) => {
                b.writeUInt16LE(1, l.pvdOffset + 128);
                b.writeUInt16BE(1, l.pvdOffset + 130);
            },
        ],
        [
            'pvd-root-extent',
            (b, l) => {
                b.writeUInt32LE(0x7fffffff, l.pvdOffset + 158);
                b.writeUInt32BE(0x7fffffff, l.pvdOffset + 162);
            },
        ],
    ],
};
const SB_FIX = { ext4: fixExt4SbCsum, btrfs: fixBtrfsSbCsum, xfs: fixXfsSbCrc };

/** 2. Zeroed metadata. */
const ZEROS = {
    ext4: [
        ['zero-gdt', (l) => [[l.gdtOffset, l.blockSize]]],
        ['zero-root-inode', (l) => [[l.rootInode, l.inodeSize]]],
        ['zero-docs-inode', (l) => [[l.docsInode, l.inodeSize]]],
    ],
    // Mounts, then the first lookup of docs/ hits a bad inode — which this
    // superblock says should panic the kernel.
    ext4panic: [['zero-docs-inode', (l) => [[l.docsInode, l.inodeSize]]]],
    vfat: [
        ['zero-fat', (l) => [[l.fatOffset, l.fatBytes]]],
        ['zero-rootdir', (l) => [[l.rootDirOffset, l.rootDirBytes]]],
    ],
    btrfs: [
        ['zero-chunk-tree', (l) => l.treeBlocks['3'].map((o) => [o, l.nodeSize])],
        ['zero-fs-tree', (l) => l.treeBlocks['5'].map((o) => [o, l.nodeSize])],
    ],
    xfs: [
        ['zero-agf', (l) => [[l.agfOffset, l.sectSize]]],
        ['zero-agi', (l) => [[l.agiOffset, l.sectSize]]],
        ['zero-root-inode', (l) => [[l.rootInode, l.inodeSize]]],
    ],
    iso9660: [
        ['zero-rootdir', (l) => [[l.rootDirOffset, l.blockSize]]],
        // Linux isofs never reads path tables; the docs/ extent is read on lookup.
        ['zero-docs-dir', (l) => [[l.docsDirOffset, l.blockSize]]],
    ],
};

/** 3 + 4. Byte flips and truncation run over these. */
const FLIP_TRUNC = ['ext4', 'vfat', 'btrfs', 'xfs', 'iso9660'];

const MBR = (i) => 446 + 16 * i;
const gptEntry = (b, i) => Number(b.readBigUInt64LE(512 + 72)) * 512 + i * b.readUInt32LE(512 + 84);

/** 5 + 6. Container headers and partition tables: [name, edit, fix?]. */
const HEADER_EDITS = {
    qcow2: [
        ['l1-offset', (b) => b.writeBigUInt64BE(0x7ffffffffff00000n, 40)],
        ['refcount-offset', (b) => b.writeBigUInt64BE(0x7ffffffffff00000n, 48)],
        ['cluster-bits', (b) => b.writeUInt32BE(31, 20)],
        // Opens fine and most of the disk stays intact: L2 entry 1 (guest
        // bytes 64-128 KiB) is set to COPIED with host offset 0, so that
        // guest cluster reads the qcow2 header cluster.
        [
            'l2-entry-at-header',
            (b) => {
                const l1 = Number(b.readBigUInt64BE(40));
                const l2 = Number(b.readBigUInt64BE(l1) & 0x00fffffffffffe00n);
                b.writeBigUInt64BE(0x8000000000000000n | 0n, l2 + 8);
            },
        ],
    ],
    vmdk: [
        ['capacity', (b) => b.writeBigUInt64LE(1n << 62n, 12)],
        ['grain-size', (b) => b.writeBigUInt64LE(1n << 40n, 20)],
        ['gd-offset', (b) => b.writeBigUInt64LE(0x00ffffffffffff00n, 56)],
    ],
    mbr: [
        // p2 starts inside p1.
        ['overlap', (b) => b.writeUInt32LE(b.readUInt32LE(MBR(0) + 8) + 1024, MBR(1) + 8)],
        [
            'out-of-range',
            (b) => {
                b.writeUInt32LE(0xffff0000, MBR(1) + 8);
                b.writeUInt32LE(0xffff, MBR(1) + 12);
            },
        ],
    ],
    mbrext: [
        // The first EBR's "next" link points back at that same EBR.
        [
            'ext-loop',
            (b) => {
                const e = b.readUInt32LE(MBR(1) + 8) * 512 + MBR(1);
                b[e + 4] = 0x05;
                b.writeUInt32LE(0, e + 8);
                b.writeUInt32LE(2048, e + 12);
            },
        ],
    ],
    gpt: [
        [
            'overlap',
            (b) => {
                const start = b.readBigUInt64LE(gptEntry(b, 0) + 32);
                b.writeBigUInt64LE(start + 1024n, gptEntry(b, 1) + 32);
            },
            fixGptCrcs,
        ],
        [
            'out-of-range',
            (b) => b.writeBigUInt64LE(0xffffffffffffn, gptEntry(b, 1) + 40),
            fixGptCrcs,
        ],
    ],
};

/** Every mutated case: [{ name, base, mutation, apply(buf, layout) }]. */
export function mutationCases() {
    const out = [];
    const add = (base, mutation, apply) =>
        out.push({ name: `${base}-${mutation}`, base, mutation, apply });
    for (const [base, edits] of Object.entries(SB_EDITS)) {
        for (const [m, fn] of edits) add(base, m, edit(fn, SB_FIX[base]));
    }
    for (const [base, zs] of Object.entries(ZEROS)) {
        for (const [m, pick] of zs) add(base, m, zero(pick));
    }
    for (const base of FLIP_TRUNC) {
        add(base, 'flip-s1-d1e-4', (b) => flip(b, 1, 1e-4));
        add(base, 'flip-s2-d1e-3', (b) => flip(b, 2, 1e-3));
        for (const pct of [25, 50, 90]) add(base, `trunc-${pct}`, (b) => truncate(b, pct));
    }
    for (const [base, edits] of Object.entries(HEADER_EDITS)) {
        for (const [m, fn, fix] of edits) add(base, m, edit(fn, fix));
    }
    return out;
}
