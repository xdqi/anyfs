/**
 * Public-API smoke test for @anyfs/core, against the *built* dist/.
 * Run after `pnpm -F @anyfs/core build`.
 *
 *   node ts/packages/core/test/api.node.mjs single
 *   node ts/packages/core/test/api.node.mjs multi
 *   node ts/packages/core/test/api.node.mjs big
 */
import path from 'node:path';
import { strict as assert } from 'node:assert';

const IMAGES = {
    single: '${LKLFTPD_SRC}/disk_single.img',
    multi: '${LKLFTPD_SRC}/disk_multi.img',
    big: '${LKLFTPD_SRC}/build/diagnostic/big_ext4.img',
};

const which = process.argv[2] || 'single';
const img = IMAGES[which];
if (!img) {
    console.error('unknown:', which);
    process.exit(2);
}

const { mountNodeFile, haltKernel } = await import(
    new URL('../dist/node.js', import.meta.url).href
);
const { default: factory } = await import(new URL('../wasm/anyfs.node.mjs', import.meta.url).href);

console.log(`[api] mounting ${img} via @anyfs/core …`);
const disk = await mountNodeFile(img, factory, { memMb: 64 });

const parts = await disk.listPartitions();
console.log(`[api] partitions: ${parts.length}`);

if (which === 'multi') {
    assert.equal(parts.length, 3, 'disk_multi should have 3 partitions');
    console.log('[api] OK multi: 3 partitions');
} else {
    assert.equal(parts.length, 0, 'whole-disk image should have 0 partitions');
    const mp = await disk.mountWhole('ext4');
    console.log(`[api] mounted whole-disk at ${mp}`);
    const entries = await disk.readdir(mp);
    console.log(`[api] readdir(${mp}) =>`, entries.map((e) => `${e.kind}:${e.name}`).join(', '));

    if (which === 'single') {
        const hello = entries.find((e) => e.name === 'hello.txt');
        assert.ok(hello, 'expected hello.txt in disk_single');
        const stat = await disk.stat(`${mp}/hello.txt`);
        console.log(`[api] stat(hello.txt) size=${stat.size}`);
        const fd = await disk.open(`${mp}/hello.txt`);
        const bytes = await disk.read(fd, 0, stat.size);
        await disk.close(fd);
        console.log(`[api] read =>`, JSON.stringify(new TextDecoder().decode(bytes)));
    } else if (which === 'big') {
        const big = entries.find((e) => e.name === 'big.bin');
        assert.ok(big, 'expected big.bin in big_ext4');
        const stat = await disk.stat(`${mp}/big.bin`);
        console.log(`[api] stat(big.bin) size=${stat.size}`);
        const fd = await disk.open(`${mp}/big.bin`);
        const head = await disk.read(fd, 0, 16);
        const tail = await disk.read(fd, stat.size - 16, 16);
        await disk.close(fd);
        console.log(`[api] head =`, Buffer.from(head).toString('hex'));
        console.log(`[api] tail =`, Buffer.from(tail).toString('hex'));
    }
}

await disk.dispose();
await haltKernel();
console.log('[api] OK');
process.exit(0);
