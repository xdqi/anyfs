/**
 * Verifies AnyfsDisk.openReadable() streams a file in chunks without
 * buffering the whole file in wasm memory.
 *
 *   node ts/packages/core/test/openReadable.node.mjs single
 *   node ts/packages/core/test/openReadable.node.mjs big
 */
import { strict as assert } from 'node:assert';

const IMAGES = {
    single: { path: '${LKLFTPD_SRC}/disk_single.img', target: 'hello.txt' },
    big: { path: '${LKLFTPD_SRC}/build/diagnostic/big_ext4.img', target: 'big.bin' },
};

const which = process.argv[2] || 'single';
const cfg = IMAGES[which];
if (!cfg) {
    console.error('unknown:', which);
    process.exit(2);
}

const { mountNodeFile, haltKernel } = await import(
    new URL('../dist/node.js', import.meta.url).href
);
const { default: factory } = await import(new URL('../wasm/anyfs.node.mjs', import.meta.url).href);

console.log(`[stream] mounting ${cfg.path} …`);
const disk = await mountNodeFile(cfg.path, factory, { memMb: 64 });
const mp = await disk.mountWhole('ext4');
const abs = `${mp}/${cfg.target}`;
const stat = await disk.stat(abs);
console.log(`[stream] ${abs} size=${stat.size}`);

const chunkSize = 1 << 20; // 1 MiB — what the demo defaults to
const { stream, size } = await disk.openReadable(abs, { chunkSize });
assert.equal(size, stat.size);

let received = 0;
let maxChunk = 0;
let chunks = 0;
const reader = stream.getReader();
let firstChunk = null;
let lastChunk = null;
for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks++;
    if (received === 0) firstChunk = value.slice(0, Math.min(16, value.length));
    lastChunk = value;
    received += value.byteLength;
    maxChunk = Math.max(maxChunk, value.byteLength);
}
console.log(`[stream] received=${received} chunks=${chunks} maxChunk=${maxChunk}`);
assert.equal(received, size, 'streamed bytes must equal file size');
assert.ok(maxChunk <= chunkSize, `chunk too large: ${maxChunk} > ${chunkSize}`);

if (which === 'single') {
    const text = new TextDecoder().decode(
        firstChunk && firstChunk.byteLength === received ? firstChunk : lastChunk,
    );
    console.log(`[stream] content =`, JSON.stringify(text));
}
if (which === 'big') {
    const tail16 = lastChunk.slice(lastChunk.byteLength - 16);
    console.log(`[stream] tail16 =`, Buffer.from(tail16).toString('hex'));
}

await disk.dispose();
await haltKernel();
console.log('[stream] OK');
process.exit(0);
