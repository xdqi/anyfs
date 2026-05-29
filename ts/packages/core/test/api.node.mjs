/**
 * Public-API smoke test for @anyfs/core, against the *built* dist/.
 * Run after `pnpm -F @anyfs/core build`.
 *
 *   node ts/packages/core/test/api.node.mjs single
 *   node ts/packages/core/test/api.node.mjs multi
 *   node ts/packages/core/test/api.node.mjs big
 */
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { strict as assert } from 'node:assert';

const DISKS_DIR = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    '../../../examples/vite-demo/public/disks',
);

const IMAGES = {
    single: path.join(DISKS_DIR, 'single.img'),
    multi: path.join(DISKS_DIR, 'multi.img'),
    big: path.join(DISKS_DIR, 'big.img'),
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
const session = await mountNodeFile(img, factory, { memMb: 64 });

const parts = await session.listParts();
console.log(`[api] parts: ${parts.length}`);

if (which === 'multi') {
    assert.equal(parts.length, 3, 'disk_multi should have 3 parts');
    console.log('[api] OK multi: 3 parts');
} else {
    assert.equal(parts.length, 0, 'whole-disk image should have 0 parts');
    const mp = await session.enter(0);
    console.log(`[api] mounted whole-disk at ${mp}`);
    const entries = await session.readdir(mp);
    console.log(`[api] readdir(${mp}) =>`, entries.map((e) => `${e.kind}:${e.name}`).join(', '));

    if (which === 'single') {
        const hello = entries.find((e) => e.name === 'hello.txt');
        assert.ok(hello, 'expected hello.txt in disk_single');
        const st = await session.stat(`${mp}/hello.txt`);
        console.log(`[api] stat(hello.txt) size=${st.size}`);
        const fd = await session.openFd(`${mp}/hello.txt`);
        const bytes = await session.readFd(fd, 0, st.size);
        await session.closeFd(fd);
        console.log(`[api] read =>`, JSON.stringify(new TextDecoder().decode(bytes)));
    } else if (which === 'big') {
        const big = entries.find((e) => e.name === 'big.bin');
        assert.ok(big, 'expected big.bin in big_ext4');
        const st = await session.stat(`${mp}/big.bin`);
        console.log(`[api] stat(big.bin) size=${st.size}`);
        const fd = await session.openFd(`${mp}/big.bin`);
        const head = await session.readFd(fd, 0, 16);
        const tail = await session.readFd(fd, st.size - 16, 16);
        await session.closeFd(fd);
        console.log(`[api] head =`, Buffer.from(head).toString('hex'));
        console.log(`[api] tail =`, Buffer.from(tail).toString('hex'));
    }
}

await session.close();
await haltKernel();
console.log('[api] OK');
process.exit(0);
