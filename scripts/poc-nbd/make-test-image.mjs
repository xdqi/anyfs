/*
 * Build a deterministic qcow2 test image for the NBD-over-fd PoC.
 *
 * Strategy: synthesize a raw image filled with a deterministic byte
 * pattern (no filesystem tooling needed), then convert it to qcow2 with
 * qemu-img. We embed a recognizable marker at a known offset and record
 * it as the verification anchor. Stage 2 reads that marker back through
 * the qcow2-over-NBD chain to prove byte-accurate delivery.
 */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.join(here, 'fixtures');
const rawImg = path.join(outDir, 'test.raw');
const qcow = path.join(outDir, 'test.qcow2');

const SIZE = 8 * 1024 * 1024; /* 8 MiB — small, fast to convert */
const VERIFY_OFFSET = 0x100000; /* 1 MiB in: well past the qcow2 header region */
const MARKER = Buffer.from('ANYFS-NBD-POC-MARKER-v1', 'ascii');

fs.mkdirSync(outDir, { recursive: true });

/* Deterministic fill: byte i = i & 0xff, so every offset has a predictable
 * value. Then stamp the ASCII marker at VERIFY_OFFSET. */
const raw = Buffer.alloc(SIZE);
for (let i = 0; i < SIZE; i++) raw[i] = i & 0xff;
MARKER.copy(raw, VERIFY_OFFSET);
fs.writeFileSync(rawImg, raw);

execFileSync('qemu-img', ['convert', '-f', 'raw', '-O', 'qcow2', rawImg, qcow], {
  stdio: 'inherit',
});

const meta = {
  qcow2: qcow,
  raw: rawImg,
  size: SIZE,
  verifyOffset: VERIFY_OFFSET,
  verifyBytesHex: MARKER.toString('hex'),
  verifyAscii: MARKER.toString('ascii'),
};
fs.writeFileSync(path.join(outDir, 'meta.json'), JSON.stringify(meta, null, 2));
console.log(JSON.stringify(meta, null, 2));
