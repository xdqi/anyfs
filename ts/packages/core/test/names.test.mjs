import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    hasEscapedBytes,
    nameToBytes,
    displayName,
    defaultLegacyEncoding,
    fatCodepageFlag,
    MOUNT_FAT_CP_437,
    MOUNT_FAT_CP_936,
    MOUNT_FAT_CP_950,
    MOUNT_FAT_CP_932,
    MOUNT_FAT_CP_949,
} from '../dist/index.js';

// How the C glue escapes a byte that is not UTF-8 (src/core/anyfs_name.c).
const esc = (...bytes) => bytes.map((b) => String.fromCodePoint(0xef00 + b)).join('');
// A legacy name as the glue hands it over when none of its byte runs happen
// to be valid multi-byte UTF-8: ASCII bytes stay, the others are escaped.
const legacy = (...bytes) =>
    bytes.map((b) => (b < 0x80 ? String.fromCharCode(b) : esc(b))).join('');

test('hasEscapedBytes', () => {
    assert.equal(hasEscapedBytes('plain.txt'), false);
    assert.equal(hasEscapedBytes('中文.txt'), false);
    assert.equal(hasEscapedBytes(`caf${esc(0xe9)}.txt`), true);
});

test('nameToBytes matches the C escape vectors', () => {
    assert.deepEqual([...nameToBytes('plain')], [...Buffer.from('plain')]);
    assert.deepEqual([...nameToBytes('中文')], [0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87]);
    assert.deepEqual([...nameToBytes(`caf${esc(0xe9)}`)], [0x63, 0x61, 0x66, 0xe9]);
    // A real U+EF80 travels as its own three bytes, escaped.
    assert.deepEqual([...nameToBytes(esc(0xee, 0xbe, 0x80))], [0xee, 0xbe, 0x80]);
    assert.deepEqual([...nameToBytes(esc(0xed, 0xa0, 0x80))], [0xed, 0xa0, 0x80]);
});

test('displayName: UTF-8 names are shown as they are', () => {
    for (const enc of ['gb18030', 'big5', 'shift_jis', 'euc-kr', 'windows-1252', 'off'])
        assert.equal(displayName('中文 café.txt', enc), '中文 café.txt');
});

test('displayName: legacy names decode with the chosen encoding', () => {
    const gbk = `${esc(0xd6, 0xd0, 0xce, 0xc4)}.txt`; // 中文 in GBK
    assert.equal(displayName(gbk, 'gb18030'), '中文.txt');
    assert.equal(displayName(`caf${esc(0xe9)}.txt`, 'windows-1252'), 'café.txt');
    assert.equal(displayName(`${esc(0xa4, 0xa4, 0xa4, 0xe5)}`, 'big5'), '中文');
    assert.equal(displayName(legacy(0x83, 0x65, 0x83, 0x58, 0x83, 0x67), 'shift_jis'), 'テスト');
    assert.equal(displayName(`${esc(0xc7, 0xd1)}`, 'euc-kr'), '한');
});

test('displayName: the whole name is decoded, not just the escaped runs', () => {
    // GBK 中模 is D6 D0 C4 A3, and C4 A3 is also valid UTF-8 (U+0123 ģ).
    // The C side lets valid UTF-8 through, so the name arrives part escaped,
    // part literal; decoding must use all of its bytes.
    const name = `${esc(0xd6, 0xd0)}ģ`; // D6 D0 C4 A3
    assert.equal(displayName(name, 'gb18030'), '中模');
});

test('displayName: \\xNN when the encoding does not fit or is off', () => {
    const gbk = `${esc(0xd6, 0xd0, 0xce, 0xc4)}.txt`;
    assert.equal(displayName(gbk, 'off'), '\\xD6\\xD0\\xCE\\xC4.txt');
    assert.equal(displayName(`a${esc(0xff)}`, 'shift_jis'), 'a\\xFF');
    assert.equal(displayName(`a${esc(0xff)}b`, 'off'), 'a\\xFFb');
});

test('defaultLegacyEncoding follows the language', () => {
    for (const l of ['zh-CN', 'zh', 'zh-SG', 'zh-Hans', 'zh-Hans-CN'])
        assert.equal(defaultLegacyEncoding(l), 'gb18030');
    for (const l of ['zh-TW', 'zh-HK', 'zh-MO', 'zh-Hant', 'zh-Hant-TW'])
        assert.equal(defaultLegacyEncoding(l), 'big5');
    assert.equal(defaultLegacyEncoding('ja'), 'shift_jis');
    assert.equal(defaultLegacyEncoding('ja-JP'), 'shift_jis');
    assert.equal(defaultLegacyEncoding('ko-KR'), 'euc-kr');
    assert.equal(defaultLegacyEncoding('en-US'), 'windows-1252');
    assert.equal(defaultLegacyEncoding('de'), 'windows-1252');
    assert.equal(defaultLegacyEncoding(''), 'windows-1252');
});

test('fatCodepageFlag maps to the C enter flags', () => {
    assert.equal(MOUNT_FAT_CP_437, 0);
    assert.equal(MOUNT_FAT_CP_936, 1 << 8);
    assert.equal(fatCodepageFlag('gb18030'), MOUNT_FAT_CP_936);
    assert.equal(fatCodepageFlag('big5'), MOUNT_FAT_CP_950);
    assert.equal(fatCodepageFlag('shift_jis'), MOUNT_FAT_CP_932);
    assert.equal(fatCodepageFlag('euc-kr'), MOUNT_FAT_CP_949);
    assert.equal(fatCodepageFlag('windows-1252'), MOUNT_FAT_CP_437);
    assert.equal(fatCodepageFlag('off'), MOUNT_FAT_CP_437);
});
