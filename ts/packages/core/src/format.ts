/** Format byte count as human-readable string (raw + readable). */
export function fmtBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${n} B (${(n / 1024).toFixed(1)} KiB)`;
    if (n < 1024 * 1024 * 1024) return `${n} B (${(n / 1024 / 1024).toFixed(1)} MiB)`;
    return `${n} B (${(n / 1024 / 1024 / 1024).toFixed(2)} GiB)`;
}

/** Format POSIX mode bits as "drwxr-xr-x (0755)". */
export function fmtMode(mode: number): string {
    const types: Array<[number, string]> = [
        [0o140000, 's'], // socket
        [0o120000, 'l'], // symlink
        [0o100000, '-'], // regular
        [0o060000, 'b'], // block dev
        [0o040000, 'd'], // dir
        [0o020000, 'c'], // char dev
        [0o010000, 'p'], // fifo
    ];
    let typeCh = '?';
    for (const [m, ch] of types) {
        if ((mode & 0o170000) === m) {
            typeCh = ch;
            break;
        }
    }
    const perm = (bits: number, suid: boolean, gid: boolean, sticky: boolean) => {
        const r = bits & 4 ? 'r' : '-';
        const w = bits & 2 ? 'w' : '-';
        let x = bits & 1 ? 'x' : '-';
        if (suid) x = bits & 1 ? 's' : 'S';
        if (gid) x = bits & 1 ? 's' : 'S';
        if (sticky) x = bits & 1 ? 't' : 'T';
        return r + w + x;
    };
    const u = perm((mode >> 6) & 7, !!(mode & 0o4000), false, false);
    const g = perm((mode >> 3) & 7, false, !!(mode & 0o2000), false);
    const o = perm(mode & 7, false, false, !!(mode & 0o1000));
    return `${typeCh}${u}${g}${o} (0${(mode & 0o7777).toString(8)})`;
}

/** Format a Unix timestamp (seconds) as human-readable date + epoch. */
export function fmtTime(sec: number): string {
    if (!sec) return '—';
    const d = new Date(sec * 1000);
    return `${d
        .toISOString()
        .replace('T', ' ')
        .replace(/\.\d+Z$/, ' UTC')} (epoch ${sec})`;
}

/** Format Linux dev_t as "major:minor (raw)". */
export function fmtDev(dev: number): string {
    const major = ((dev >>> 8) & 0xfff) | ((Math.floor(dev / 0x100000000) >>> 0) & 0xfffff000);
    const minor = (dev & 0xff) | ((dev >>> 12) & 0xffffff00);
    return `${major}:${minor} (${dev})`;
}

/** Format a raw number of bytes with adaptive units (used by Recents/disk summary). */
export function formatSize(n: number | undefined): string {
    if (n === undefined || !Number.isFinite(n)) return '';
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let v = n;
    let u = 0;
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024;
        u++;
    }
    return `${v < 10 && u > 0 ? v.toFixed(1) : Math.round(v)} ${units[u]}`;
}

/**
 * Split a filename's extension.
 * Rules:
 *   - no dot → no extension (`""`)
 *   - leading dot (dotfile like `.pwd.lock`) → only split on a *later* dot
 *   - trailing dot → no extension
 */
export function splitExt(name: string): string {
    const i = name.lastIndexOf('.');
    if (i <= 0) return '';
    if (i === name.length - 1) return '';
    return name.substring(i);
}

const GPT_ROLES: Record<string, string> = {
    'c12a7328-f81f-11d2-ba4b-00a0c93ec93b': 'EFI System',
    '21686148-6449-6e6f-744e-656564454649': 'BIOS boot',
    '0fc63daf-8483-4772-8e79-3d69d8477de4': 'Linux filesystem',
    '44479540-f297-41b2-9af7-d131d5f0458a': 'Linux root (x86)',
    '4f68bce3-e8cd-4db1-96e7-fbcaf984b709': 'Linux root (x86-64)',
    'b921b045-1df0-41c3-af44-4c6f280d3fae': 'Linux root (ARM64)',
    'bc13c2ff-59e6-4262-a352-b275fd6f7172': 'Linux extended boot',
    '933ac7e1-2eb4-4f13-b844-0e14e2aef915': 'Linux home',
    '0657fd6d-a4ab-43c4-84e5-0933c84b4f4f': 'Linux swap',
    'e6d6d379-f507-44c2-a23c-238f2a3df928': 'Linux LVM',
    'a19d880f-05fc-4d3b-a006-743f0f84911e': 'Linux RAID',
    'ca7d7ccb-63ed-4c53-861c-1742536059cc': 'Linux LUKS',
    'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7': 'Microsoft basic data',
    'e3c9e316-0b5c-4db8-817d-f92df00215ae': 'Microsoft reserved',
    'de94bba4-06d1-4d40-a16a-bfd50179d6ac': 'Windows recovery',
    '48465300-0000-11aa-aa11-00306543ecac': 'Apple HFS+',
    '7c3457ef-0000-11aa-aa11-00306543ecac': 'Apple APFS',
    '516e7cb6-6ecf-11d6-8ff8-00022d09712b': 'FreeBSD UFS',
    '6a898cc3-1dd2-11b2-99a6-080020736631': 'ZFS',
};

const MBR_ROLES: Record<number, string> = {
    0x01: 'FAT12',
    0x04: 'FAT16',
    0x06: 'FAT16',
    0x07: 'NTFS/exFAT',
    0x0b: 'FAT32',
    0x0c: 'FAT32',
    0x0e: 'FAT16',
    0x27: 'Windows recovery',
    0x82: 'Linux swap',
    0x83: 'Linux',
    0x8e: 'Linux LVM',
    0xa5: 'FreeBSD',
    0xa8: 'Apple UFS',
    0xaf: 'Apple HFS+',
    0xef: 'EFI System',
    0xfd: 'Linux RAID',
};

/**
 * Role of a partition from its partition-table type (`SessionPartInfo.ptype`:
 * MBR `"0x83"` or a GPT type GUID). This is what the table *declares* the
 * partition is for, not what libblkid found on it — e.g. "BIOS boot" holds a
 * bootloader and no filesystem. `""` for unknown or unrecognised types.
 */
export function partitionRole(ptype: string): string {
    const t = ptype.trim().toLowerCase();
    if (/^0x[0-9a-f]{1,2}$/.test(t)) return MBR_ROLES[Number.parseInt(t, 16)] ?? '';
    return GPT_ROLES[t] ?? '';
}
