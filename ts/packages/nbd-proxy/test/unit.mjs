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

/* NbdServer: out-of-order completion must reply keyed by handle.
 * Drive serveNbd over an in-process TCP pair with a mock DataSource whose
 * reads resolve in REVERSE order (later request finishes first). */
await test('NbdServer replies keyed by handle under out-of-order completion', async () => {
  const net = await import('node:net');
  const { serveNbd } = await import(dist);

  const SIZE = 1 << 20;
  const source = {
    async size() {
      return SIZE;
    },
    async read(offset, length) {
      const delay = offset === 0 ? 60 : 5; /* offset 0 finishes LAST */
      await new Promise((r) => setTimeout(r, delay));
      const b = Buffer.alloc(length);
      b.writeUInt32BE(offset >>> 0, 0);
      return b;
    },
    async close() {},
  };

  const server = net.createServer((sock) => {
    serveNbd(sock, source, { size: SIZE }).catch(() => {});
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;

  const replies = await new Promise((resolve, reject) => {
    const c = net.connect(port, '127.0.0.1');
    const got = [];
    let phase = 'hello';
    let buf = Buffer.alloc(0);
    c.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (phase === 'hello' && buf.length >= 18) {
        buf = buf.subarray(18);
        c.write(Buffer.alloc(4)); /* client flags */
        const o = Buffer.alloc(16);
        o.writeBigUInt64BE(0x49484156454f5054n, 0);
        o.writeUInt32BE(7, 8); /* NBD_OPT_GO */
        o.writeUInt32BE(0, 12);
        c.write(o);
        phase = 'opt';
      }
      if (phase === 'opt') {
        /* consume INFO reply (20+12) + ACK reply (20), then send two READs */
        if (buf.length >= 20 + 12 + 20) {
          buf = buf.subarray(20 + 12 + 20);
          phase = 'xmit';
          for (const off of [0, 65536]) {
            const req = Buffer.alloc(28);
            req.writeUInt32BE(0x25609513, 0);
            req.writeUInt16BE(0, 4);
            req.writeUInt16BE(0, 6); /* CMD_READ */
            req.writeUInt32BE(off, 12); /* low 4 bytes of handle = offset id */
            req.writeBigUInt64BE(BigInt(off), 16);
            req.writeUInt32BE(8, 24); /* length */
            c.write(req);
          }
        }
      }
      if (phase === 'xmit') {
        while (buf.length >= 24) {
          /* reply: 16-byte header + 8 bytes data */
          const handle = buf.readUInt32BE(12); /* low 4 bytes of handle */
          const dataOff = buf.readUInt32BE(16); /* our tagged offset */
          got.push({ handle, dataOff });
          buf = buf.subarray(24);
          if (got.length === 2) {
            c.end();
            resolve(got);
          }
        }
      }
    });
    c.on('error', reject);
    setTimeout(() => reject(new Error('timeout')), 3000);
  });
  server.close();

  for (const r of replies) {
    assert.strictEqual(r.handle, r.dataOff, 'handle must pair with its data');
  }
  assert.strictEqual(replies[0].handle, 65536, 'faster read replied first');
});

console.log(`\n${passed} test(s) passed`);
