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
