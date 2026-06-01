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

/* HttpSource: local Range server backed by the fixture, served through the
 * proxy; verify qcow2 detection + that range reads REUSE connections. */
{
  const http = await import('node:http');
  const { createDataSource, serveOnLoopback } = await import(dist);
  const raw = fs.readFileSync(meta.qcow2);
  let connections = 0;
  const upstream = http.createServer((req, res) => {
    if (req.method === 'HEAD') {
      res.writeHead(200, {
        'content-length': String(raw.length),
        'accept-ranges': 'bytes',
      });
      return res.end();
    }
    const m = /bytes=(\d+)-(\d+)/.exec(req.headers.range || '');
    if (!m) {
      res.writeHead(200, { 'content-length': String(raw.length) });
      return res.end(raw);
    }
    const start = +m[1];
    const end = +m[2];
    res.writeHead(206, {
      'content-range': `bytes ${start}-${end}/${raw.length}`,
      'content-length': String(end - start + 1),
      'accept-ranges': 'bytes',
    });
    res.end(raw.subarray(start, end + 1));
  });
  upstream.on('connection', () => connections++);
  await new Promise((r) => upstream.listen(0, '127.0.0.1', r));
  const upPort = upstream.address().port;

  const src = await createDataSource({
    kind: 'url',
    target: `http://127.0.0.1:${upPort}/disk.qcow2`,
  });
  const { port, stop } = await serveOnLoopback(src);
  try {
    const { stdout } = await execFileP('qemu-img', ['info', `nbd://127.0.0.1:${port}`], {
      encoding: 'utf8',
    });
    if (!/file format:\s*qcow2/.test(stdout)) {
      console.error('FAIL: HttpSource — qcow2 not detected through proxy');
      fail = true;
    }
    console.log(`HttpSource: upstream TCP connections opened = ${connections}`);
    if (connections > 4) {
      console.error(`FAIL: HttpSource opened ${connections} connections (no keep-alive reuse)`);
      fail = true;
    }
  } finally {
    await stop();
    await src.close();
    await new Promise((r) => upstream.close(r));
  }
  console.log('HttpSource case done');
}

if (fail) process.exit(1);
console.log(`\nINTEGRATION PASS: qcow2 detected + marker "${meta.verifyAscii}" verified through proxy (FileSource + HttpSource)`);
