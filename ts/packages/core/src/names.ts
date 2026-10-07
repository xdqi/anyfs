/**
 * File names as the engine hands them to JavaScript.
 *
 * Kernel file names are bytes. The C glue (`ts/native/anyfs_ts.c`) passes
 * strict UTF-8 through and maps every other byte `b` to the private-use
 * character U+EF00 + b (U+EF80–U+EFFF); paths coming back are mapped the
 * other way. So a name from `readdir` always opens again as it is, and these
 * helpers only matter for showing it: a name with escaped bytes was written
 * in a legacy encoding (GBK, Big5, Shift_JIS, …) and is decoded with one.
 */

export type LegacyEncoding = 'gb18030' | 'big5' | 'shift_jis' | 'euc-kr' | 'windows-1252' | 'off';

/** `enter()` flags selecting the codepage of FAT short (8.3) names; mirror
 *  `ANYFS_MOUNT_FAT_CP_*` in `include/anyfs.h`. */
export const MOUNT_FAT_CP_437 = 0;
export const MOUNT_FAT_CP_936 = 1 << 8;
export const MOUNT_FAT_CP_950 = 2 << 8;
export const MOUNT_FAT_CP_932 = 3 << 8;
export const MOUNT_FAT_CP_949 = 4 << 8;

const ESC_FIRST = 0xef80;
const ESC_LAST = 0xefff;

const isEscape = (cp: number) => cp >= ESC_FIRST && cp <= ESC_LAST;

/** Whether the name carries bytes that are not UTF-8. */
export function hasEscapedBytes(name: string): boolean {
    for (const ch of name) if (isEscape(ch.codePointAt(0)!)) return true;
    return false;
}

const utf8 = new TextEncoder();

/** The name's bytes on disk. */
export function nameToBytes(name: string): Uint8Array {
    const out: number[] = [];
    for (const ch of name) {
        const cp = ch.codePointAt(0)!;
        if (isEscape(cp)) out.push(cp - 0xef00);
        else out.push(...utf8.encode(ch));
    }
    return Uint8Array.from(out);
}

function hexEscaped(name: string): string {
    let s = '';
    for (const ch of name) {
        const cp = ch.codePointAt(0)!;
        s += isEscape(cp) ? `\\x${(cp - 0xef00).toString(16).toUpperCase().padStart(2, '0')}` : ch;
    }
    return s;
}

/**
 * A name for display. UTF-8 names are returned as they are. A name with
 * escaped bytes is decoded as a whole with `enc` (its valid-looking parts
 * may be legacy bytes too, e.g. GBK `C4 A3`); if that fails, or `enc` is
 * `'off'`, each escaped byte shows as `\xNN`.
 */
export function displayName(name: string, enc: LegacyEncoding): string {
    if (!hasEscapedBytes(name)) return name;
    if (enc !== 'off') {
        try {
            return new TextDecoder(enc, { fatal: true }).decode(nameToBytes(name));
        } catch {
            /* not this encoding */
        }
    }
    return hexEscaped(name);
}

/** The legacy encoding most likely for a UI language (BCP 47 tag). */
export function defaultLegacyEncoding(lang: string): Exclude<LegacyEncoding, 'off'> {
    const l = lang.toLowerCase();
    if (/^zh(-hant|-tw|-hk|-mo)/.test(l)) return 'big5';
    if (/^zh(\b|-)/.test(l) || l === 'zh') return 'gb18030';
    if (/^ja(\b|-)/.test(l)) return 'shift_jis';
    if (/^ko(\b|-)/.test(l)) return 'euc-kr';
    return 'windows-1252';
}

/** The `enter()` flag giving FAT short names the encoding's OEM codepage. */
export function fatCodepageFlag(enc: LegacyEncoding): number {
    switch (enc) {
        case 'gb18030':
            return MOUNT_FAT_CP_936;
        case 'big5':
            return MOUNT_FAT_CP_950;
        case 'shift_jis':
            return MOUNT_FAT_CP_932;
        case 'euc-kr':
            return MOUNT_FAT_CP_949;
        default:
            return MOUNT_FAT_CP_437;
    }
}
