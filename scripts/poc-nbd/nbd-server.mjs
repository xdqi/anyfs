/*
 * Hand-written read-only NBD newstyle server for the NBD-over-fd PoC.
 *
 * PoC scope: synchronous fs.readSync, one request at a time. The
 * production server (spec §9) is fully async with multiple in-flight
 * requests and a keep-alive http upstream — out of scope here.
 *
 * startNbdServer(socket, imageFd, size, onRead?) drives one client on an
 * already-connected socket. Returns a promise that resolves on clean
 * NBD_CMD_DISC / EOF.
 */
import { Buffer } from 'node:buffer';
import fs from 'node:fs';

const NBDMAGIC = 0x4e42444d41474943n;
const IHAVEOPT = 0x49484156454f5054n;
const OPT_REPLY_MAGIC = 0x3e889045565a9700n;
const REQ_MAGIC = 0x25609513;
const SIMPLE_REPLY_MAGIC = 0x67446698;

const NBD_FLAG_FIXED_NEWSTYLE = 1;
const NBD_FLAG_NO_ZEROES = 2;

const NBD_OPT_ABORT = 2;
const NBD_OPT_INFO = 6;
const NBD_OPT_GO = 7;

const NBD_REP_ACK = 1;
const NBD_REP_INFO = 3;
const NBD_REP_FLAG_ERROR = 0x80000000;
const NBD_REP_ERR_UNSUP = (NBD_REP_FLAG_ERROR | 1) >>> 0;

const NBD_INFO_EXPORT = 0;
const NBD_FLAG_HAS_FLAGS = 1;
const NBD_FLAG_READ_ONLY = 2;

const NBD_CMD_READ = 0;
const NBD_CMD_DISC = 2;
const NBD_CMD_FLUSH = 3;

const EINVAL = 22;

/* Read exactly n bytes from a socket, buffering across 'data' events. */
function makeReader(socket) {
  let chunks = [];
  let buffered = 0;
  let want = 0;
  let resolveWant = null;
  let rejectWant = null;
  let ended = false;
  let errored = null;

  function tryResolve() {
    if (resolveWant && buffered >= want) {
      const all = Buffer.concat(chunks);
      const out = all.subarray(0, want);
      const rest = all.subarray(want);
      chunks = rest.length ? [Buffer.from(rest)] : [];
      buffered = rest.length;
      const r = resolveWant;
      resolveWant = null;
      rejectWant = null;
      r(out);
    } else if (resolveWant && (ended || errored)) {
      const rej = rejectWant;
      resolveWant = null;
      rejectWant = null;
      rej(errored || new Error('EOF'));
    }
  }

  socket.on('data', (d) => {
    chunks.push(d);
    buffered += d.length;
    tryResolve();
  });
  socket.on('end', () => {
    ended = true;
    tryResolve();
  });
  socket.on('error', (e) => {
    errored = e;
    tryResolve();
  });

  return (n) =>
    new Promise((resolve, reject) => {
      want = n;
      resolveWant = resolve;
      rejectWant = reject;
      tryResolve();
    });
}

export async function startNbdServer(socket, imageFd, size, onRead) {
  const read = makeReader(socket);
  const write = (buf) =>
    new Promise((res, rej) => socket.write(buf, (e) => (e ? rej(e) : res())));

  /* --- Handshake --- */
  const hello = Buffer.alloc(18);
  hello.writeBigUInt64BE(NBDMAGIC, 0);
  hello.writeBigUInt64BE(IHAVEOPT, 8);
  hello.writeUInt16BE(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES, 16);
  await write(hello);

  await read(4); /* client flags (ignored for PoC) */

  /* --- Option haggling: loop until GO (enter transmission) or ABORT --- */
  for (;;) {
    const optHdr = await read(16);
    const optMagic = optHdr.readBigUInt64BE(0);
    const opt = optHdr.readUInt32BE(8);
    const optLen = optHdr.readUInt32BE(12);
    if (optMagic !== IHAVEOPT) throw new Error('bad option magic');
    if (optLen) await read(optLen); /* discard option data (export name etc.) */

    if (opt === NBD_OPT_GO || opt === NBD_OPT_INFO) {
      /* NBD_REP_INFO payload: info type (u16) + size (u64) + flags (u16) */
      const infoReply = Buffer.alloc(2 + 8 + 2);
      infoReply.writeUInt16BE(NBD_INFO_EXPORT, 0);
      infoReply.writeBigUInt64BE(BigInt(size), 2);
      infoReply.writeUInt16BE(NBD_FLAG_HAS_FLAGS | NBD_FLAG_READ_ONLY, 10);
      await sendOptReply(write, opt, NBD_REP_INFO, infoReply);
      await sendOptReply(write, opt, NBD_REP_ACK, Buffer.alloc(0));
      if (opt === NBD_OPT_GO) break; /* GO enters transmission phase */
    } else if (opt === NBD_OPT_ABORT) {
      await sendOptReply(write, opt, NBD_REP_ACK, Buffer.alloc(0));
      socket.end();
      return;
    } else {
      await sendOptReply(write, opt, NBD_REP_ERR_UNSUP, Buffer.alloc(0));
    }
  }

  /* --- Transmission phase --- */
  for (;;) {
    let hdr;
    try {
      hdr = await read(28);
    } catch {
      return; /* EOF / disconnect */
    }
    const magic = hdr.readUInt32BE(0);
    if (magic !== REQ_MAGIC) throw new Error('bad request magic');
    const type = hdr.readUInt16BE(6);
    const handle = hdr.subarray(8, 16); /* opaque 8 bytes */
    const offset = hdr.readBigUInt64BE(16);
    const length = hdr.readUInt32BE(24);

    if (type === NBD_CMD_DISC) return;

    if (type === NBD_CMD_READ) {
      if (offset + BigInt(length) > BigInt(size)) {
        await sendSimpleReply(write, EINVAL, handle, null);
        continue;
      }
      const data = Buffer.alloc(length);
      fs.readSync(imageFd, data, 0, length, Number(offset));
      if (onRead) onRead(Number(offset), length);
      await sendSimpleReply(write, 0, handle, data);
    } else if (type === NBD_CMD_FLUSH) {
      await sendSimpleReply(write, 0, handle, null);
    } else {
      await sendSimpleReply(write, EINVAL, handle, null);
    }
  }
}

/* NBD option reply: magic(8) + opt(4) + reptype(4) + len(4) + payload */
async function sendOptReply(write, opt, repType, payload) {
  const fixed = Buffer.alloc(16);
  fixed.writeBigUInt64BE(OPT_REPLY_MAGIC, 0);
  fixed.writeUInt32BE(opt, 8);
  fixed.writeUInt32BE(repType >>> 0, 12);
  const lenBuf = Buffer.alloc(4);
  lenBuf.writeUInt32BE(payload.length, 0);
  await write(Buffer.concat([fixed, lenBuf, payload]));
}

/* NBD simple reply: magic(4) + error(4) + handle(8) + [data] */
async function sendSimpleReply(write, error, handle, data) {
  const hdr = Buffer.alloc(16);
  hdr.writeUInt32BE(SIMPLE_REPLY_MAGIC, 0);
  hdr.writeUInt32BE(error >>> 0, 4);
  handle.copy(hdr, 8);
  await write(data ? Buffer.concat([hdr, data]) : hdr);
}
