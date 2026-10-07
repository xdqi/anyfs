import { test, expect } from '../lib/test-fixture';
import { ensureFixture } from '../fixtures/ensure';
import { setElectronImage } from '../lib/electron-image';

// File names in every encoding must list, display readably and download
// (spec docs/superpowers/specs/2026-10-07-unicode-filenames-design.md).
// Before the fix: FAT long names came out through iso8859-1 ("??.txt",
// "caf\xE9.txt"), and an ext4 name that is not UTF-8 reached JS as U+FFFD
// and could not be opened. The image is tests/make_names_image.py.
const fx = ensureFixture('names');

// The ext4 GBK name D6 D0 CE C4 .txt as the engine reports it: bytes that
// are not UTF-8 travel as U+EF00 + byte.
const gbkRaw = '\uEFD6\uEFD0\uEFCE\uEFC4.txt';

test.beforeEach(() => setElectronImage(fx.file));

test('names in every encoding list, display and download', async ({ driver }) => {
    await driver.setLegacyEncoding('gb18030');
    await driver.openImage(fx);
    // The FAT volume label is GBK bytes too.
    expect(await driver.partitionLabel(1)).toBe('测试');

    // FAT: long names are UTF-16 on disk; the 8.3-only name is GBK bytes,
    // decoded with the FAT codepage the legacy encoding selects (936).
    await driver.enterPartition(1);
    expect(await driver.listDisplayNames()).toEqual(
        expect.arrayContaining(['中文.txt', 'café.txt', '测试.TXT']),
    );
    const fat = await driver.download('中文.txt');
    expect(new TextDecoder().decode(fat.bytes)).toBe('fat-cn\n');
    expect(fat.fileName).toBe('中文.txt');
    await driver.backToPartitions();

    // ext4: byte names. The GBK one displays as 中文.txt, keeps its raw name
    // as its identity, downloads its bytes, and is saved under the
    // displayed name.
    await driver.enterPartition(2);
    const shown = await driver.listDisplayNames();
    expect(shown).toEqual(expect.arrayContaining(['中文.txt', 'plain.txt']));
    expect(shown.some((n) => n.includes('�'))).toBe(false);
    const gbk = await driver.download(gbkRaw, '中文.txt');
    expect(new TextDecoder().decode(gbk.bytes)).toBe('ext4-gbk\n');
    expect(gbk.fileName).toBe('中文.txt');
});
