/* Unit tests for @anyfs/nbd-proxy DataSource backends.
 * Run against the built dist/ (tsup output). */
import assert from 'node:assert';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dist = path.join(here, '..', 'dist', 'src', 'index.js');

const { createDataSource } = await import(dist);

let passed = 0;
async function test(name, fn) {
  await fn();
  console.log('ok -', name);
  passed++;
}

/* FileSource: deterministic byte pattern */
await test('FileSource reads exact bytes', async () => {
  const tmp = path.join(os.tmpdir(), 'nbdproxy-file-test.bin');
  const data = Buffer.alloc(4096);
  for (let i = 0; i < data.length; i++) data[i] = i & 0xff;
  fs.writeFileSync(tmp, data);

  const src = await createDataSource({ kind: 'file', target: tmp });
  assert.strictEqual(await src.size(), 4096);

  const slice = await src.read(1000, 16);
  for (let i = 0; i < 16; i++) {
    assert.strictEqual(slice[i], (1000 + i) & 0xff, `byte ${i}`);
  }
  await src.close();
  fs.unlinkSync(tmp);
});

console.log(`\n${passed} test(s) passed`);
