/*
 * Stage 2: full Linux chain.
 *   (a) lspart over inherited-fd NBD matches lspart over the plain file.
 *   (b) the deterministic marker at meta.verifyOffset reads back
 *       byte-for-byte THROUGH the qcow2-over-NBD chain, using qemu-io as
 *       a real qcow2 client over a unix-socket NBD endpoint.
 */
import { spawn, execFileSync, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serveOnFd, serveOnUnixSocket } from './launch.mjs';

const execFileP = promisify(execFile);

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const lspart = path.join(repoRoot, 'build-anyfs-linux-amd64/src/lspart/anyfs-lspart');
const meta = JSON.parse(fs.readFileSync(path.join(here, 'fixtures/meta.json')));

function dataRows(s) {
  return s
    .split('\n')
    .filter((l) => l.trim() && !/^Usage|warning:|^PATH\b/.test(l)).length;
}

let fail = false;

/* === Check (a): lspart parity over inherited-fd NBD === */
const plainOut = execFileSync(lspart, [meta.qcow2], { encoding: 'utf8' });
const { fd1, stop, readCount, done } = serveOnFd(meta.qcow2);
const child = spawn(lspart, ['--nbd-fd', '3'], {
  stdio: ['inherit', 'pipe', 'inherit', fd1],
});
let nbdOut = '';
child.stdout.on('data', (d) => (nbdOut += d));
const code = await new Promise((r) => child.on('exit', r));
stop();
await done;

console.log('--- plain-file lspart ---\n' + plainOut);
console.log('--- nbd lspart ---\n' + nbdOut);

if (code !== 0) {
  console.error('STAGE2 FAIL: nbd lspart exited non-zero');
  fail = true;
}
if (dataRows(plainOut) !== dataRows(nbdOut)) {
  console.error(
    `STAGE2 FAIL: row count differs (plain=${dataRows(plainOut)} nbd=${dataRows(nbdOut)})`,
  );
  fail = true;
}
if (readCount() === 0) {
  console.error('STAGE2 FAIL: zero NBD reads over inherited fd');
  fail = true;
}

/* === Check (b): byte verify THROUGH qcow2-over-NBD with qemu-io === */
const sockPath = '/tmp/poc-nbd-stage2.sock';
const { server } = await serveOnUnixSocket(meta.qcow2, sockPath);
try {
  const len = Buffer.from(meta.verifyBytesHex, 'hex').length;
  const nbdUri = `json:{"driver":"qcow2","file":{"driver":"nbd","server":{"type":"unix","path":"${sockPath}"}}}`;
  /* MUST be async: the in-process NBD server runs on this event loop, so a
   * synchronous execFileSync would block it and deadlock the qemu-io read. */
  const { stdout: out } = await execFileP(
    'qemu-io',
    ['-r', '-c', `read -v ${meta.verifyOffset} ${len}`, nbdUri],
    { encoding: 'utf8' },
  );
  /* qemu-io 'read -v' prints a hexdump; collect the hex byte columns. */
  const hex = [...out.matchAll(/^[0-9a-f]{8}:\s+((?:[0-9a-f]{2}\s?)+)/gim)]
    .map((m) => m[1].replace(/\s+/g, ''))
    .join('')
    .slice(0, len * 2);
  console.log('--- qemu-io marker read (through qcow2-over-NBD) ---');
  console.log(out);
  if (hex !== meta.verifyBytesHex) {
    console.error(
      `STAGE2 FAIL: marker mismatch through chain: got ${hex} want ${meta.verifyBytesHex}`,
    );
    fail = true;
  }
} catch (e) {
  console.error('STAGE2 FAIL: qemu-io byte verify errored:', e.message);
  fail = true;
} finally {
  server.close();
}

if (fail) process.exit(1);
console.log(
  `STAGE2 PASS: lspart parity (rows=${dataRows(nbdOut)}), inherited-fd reads=${readCount()}, marker verified through qcow2-over-NBD (${meta.verifyAscii})`,
);
