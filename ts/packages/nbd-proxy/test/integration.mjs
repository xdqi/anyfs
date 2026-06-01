/* Integration: open the PoC fixture qcow2 THROUGH the proxy with the real
 * QEMU client (qemu-img + qemu-io over a 127.0.0.1 loopback NBD endpoint).
 * Proves format detection + byte-accurate delivery for FileSource.
 *
 * MUST use async execFile (the in-process NBD server shares this event loop;
 * a sync execFileSync would deadlock it). */
import assert from 'node:assert';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const execFileP = promisify(execFile);
const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../../../..');
const dist = path.join(here, '..', 'dist', 'src', 'index.js');
const metaPath = path.join(repoRoot, 'scripts/poc-nbd/fixtures/meta.json');

if (!fs.existsSync(metaPath)) {
  console.error('fixture missing; run: node scripts/poc-nbd/make-test-image.mjs');
  process.exit(1);
}
const meta = JSON.parse(fs.readFileSync(metaPath, 'utf8'));
const { createDataSource, serveOnLoopback } = await import(dist);

const source = await createDataSource({ kind: 'file', target: meta.qcow2 });
const { port, stop } = await serveOnLoopback(source);
let fail = false;
try {
  const uri = `nbd://127.0.0.1:${port}`;
  /* (a) qemu-img detects qcow2 over NBD */
  const { stdout: info } = await execFileP('qemu-img', ['info', uri], {
    encoding: 'utf8',
  });
  console.log(info);
  if (!/file format:\s*qcow2/.test(info)) {
    console.error('FAIL: qemu-img did not detect qcow2 over NBD');
    fail = true;
  }

  /* (b) qemu-io reads the marker back byte-for-byte through qcow2-over-NBD */
  const len = Buffer.from(meta.verifyBytesHex, 'hex').length;
  const qio = `json:{"driver":"qcow2","file":{"driver":"nbd","server":{"type":"inet","host":"127.0.0.1","port":"${port}"}}}`;
  const { stdout: dump } = await execFileP(
    'qemu-io',
    ['-r', '-c', `read -v ${meta.verifyOffset} ${len}`, qio],
    { encoding: 'utf8' },
  );
  const hex = [...dump.matchAll(/^[0-9a-f]{8}:\s+((?:[0-9a-f]{2}\s?)+)/gim)]
    .map((m) => m[1].replace(/\s+/g, ''))
    .join('')
    .slice(0, len * 2);
  console.log(dump);
  if (hex !== meta.verifyBytesHex) {
    console.error(`FAIL: marker mismatch: got ${hex} want ${meta.verifyBytesHex}`);
    fail = true;
  }
} finally {
  await stop();
  await source.close();
}

if (fail) process.exit(1);
console.log(`\nINTEGRATION PASS: qcow2 detected + marker "${meta.verifyAscii}" verified through proxy (FileSource)`);
