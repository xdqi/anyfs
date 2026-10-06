/**
 * Where the structures the mutations target live in each base image.
 * Computed once per base; the mutations themselves are pure functions of
 * (base bytes, layout, seed).
 */
import { run } from './tools.mjs';

const LOCATED = /located at block (\d+), offset (0x[0-9a-f]+)/;

/** Byte offset of an ext4 inode via debugfs (`spec`: `<N>` or a path). */
function ext4Inode(file, spec, blockSize) {
    const out = run('debugfs', ['-R', `imap ${spec}`, file]);
    const m = LOCATED.exec(out);
    if (!m) throw new Error(`debugfs imap ${spec}: unexpected output:\n${out}`);
    return Number(m[1]) * blockSize + Number.parseInt(m[2], 16);
}

export function probeLayout(probe, file, buf) {
    switch (probe) {
        case 'ext4': {
            const sb = buf.subarray(1024, 2048);
            const blockSize = 1024 << sb.readUInt32LE(24);
            return {
                blockSize,
                inodeSize: sb.readUInt16LE(88),
                // The group descriptor table is the block after the superblock's.
                gdtOffset: (sb.readUInt32LE(20) + 1) * blockSize,
                rootInode: ext4Inode(file, '<2>', blockSize),
                docsInode: ext4Inode(file, '/docs', blockSize),
            };
        }
        case 'vfat': {
            const bps = buf.readUInt16LE(11);
            const reserved = buf.readUInt16LE(14);
            const fats = buf[16];
            const fatSectors = buf.readUInt16LE(22);
            return {
                fatOffset: reserved * bps,
                fatBytes: fatSectors * bps,
                rootDirOffset: (reserved + fats * fatSectors) * bps,
                rootDirBytes: buf.readUInt16LE(17) * 32,
            };
        }
        case 'btrfs': {
            const sb = buf.subarray(0x10000, 0x11000);
            const fsid = sb.subarray(0x20, 0x30);
            const sectorSize = sb.readUInt32LE(0x90);
            // Tree block headers carry the fsid at 0x20 and the owning tree at
            // 0x58. The base is SYSTEM|single and METADATA|single, so the extra
            // blocks of one owner are stale COW copies. Keep them all: the live
            // root must go, and old copies must not be found as fallbacks.
            const treeBlocks = {};
            for (let off = 0; off + sectorSize <= buf.length; off += sectorSize) {
                if (off === 0x10000) continue; // the superblock carries the fsid too
                if (!buf.subarray(off + 0x20, off + 0x30).equals(fsid)) continue;
                const owner = buf.readBigUInt64LE(off + 0x58);
                if (owner > 255n) continue; // log/reloc trees: not targeted
                (treeBlocks[String(owner)] ??= []).push(off);
            }
            return { nodeSize: sb.readUInt32LE(0x94), sectorSize, treeBlocks };
        }
        case 'xfs': {
            const blockSize = buf.readUInt32BE(4);
            const sectSize = buf.readUInt16BE(102);
            const inodeSize = buf.readUInt16BE(104);
            const agBlocks = buf.readUInt32BE(84);
            const inopblog = BigInt(buf[123]);
            const agblklog = BigInt(buf[124]);
            const ino = buf.readBigUInt64BE(56);
            const agno = Number(ino >> (agblklog + inopblog));
            const agino = ino & ((1n << (agblklog + inopblog)) - 1n);
            const agbno = Number(agino >> inopblog);
            const slot = Number(agino & ((1n << inopblog) - 1n));
            return {
                sectSize,
                inodeSize,
                agfOffset: sectSize, // AGF: sector 1 of AG 0
                agiOffset: 2 * sectSize, // AGI: sector 2 of AG 0
                rootInode: (agno * agBlocks + agbno) * blockSize + slot * inodeSize,
            };
        }
        case 'iso9660': {
            const pvd = 16 * 2048;
            if (buf.toString('latin1', pvd + 1, pvd + 6) !== 'CD001') {
                throw new Error('iso9660: no primary volume descriptor at sector 16');
            }
            const blockSize = buf.readUInt16LE(pvd + 128);
            const rootDirOffset = buf.readUInt32LE(pvd + 158) * blockSize;
            const rootDirBytes = buf.readUInt32LE(pvd + 166);
            // Walk the root directory's records for the "DOCS" entry.
            let docsDirOffset = 0;
            for (let p = rootDirOffset; p < rootDirOffset + rootDirBytes; ) {
                const len = buf[p];
                if (len === 0) {
                    // Records never span sectors: skip to the next one.
                    p =
                        (Math.floor((p - rootDirOffset) / blockSize) + 1) * blockSize +
                        rootDirOffset;
                    continue;
                }
                const idLen = buf[p + 32];
                if (buf.toString('latin1', p + 33, p + 33 + idLen).toUpperCase() === 'DOCS') {
                    docsDirOffset = buf.readUInt32LE(p + 2) * blockSize;
                    break;
                }
                p += len;
            }
            if (!docsDirOffset) throw new Error('iso9660: no docs/ record in the root directory');
            return {
                pvdOffset: pvd,
                blockSize,
                rootDirOffset,
                rootDirBytes,
                docsDirOffset,
                pathTableOffset: buf.readUInt32LE(pvd + 140) * blockSize,
                pathTableBytes: buf.readUInt32LE(pvd + 132),
            };
        }
        default:
            throw new Error(`no layout probe for ${probe}`);
    }
}
