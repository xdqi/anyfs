/**
 * Deterministic mutations of the base images. Each is a pure function of
 * (base bytes, layout[, seed]) and returns a new buffer; the base is never
 * modified.
 */
import { crc32 } from 'node:zlib';
import { crc32c, crc32cRaw } from './checksum.mjs';

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
