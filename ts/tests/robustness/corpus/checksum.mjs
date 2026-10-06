/** CRC32C (Castagnoli), as ext4, btrfs and xfs use it. */
const TABLE = (() => {
    const t = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
        let c = i;
        for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0x82f63b78 : c >>> 1;
        t[i] = c >>> 0;
    }
    return t;
})();

/** The kernel's crc32c(): no final inversion. ext4 stores this form. */
export function crc32cRaw(seed, buf) {
    let c = seed >>> 0;
    for (let i = 0; i < buf.length; i++) c = TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return c >>> 0;
}

/** Standard CRC32C (init ~0, final inversion). btrfs and xfs store this. */
export const crc32c = (buf) => ~crc32cRaw(0xffffffff, buf) >>> 0;
