import { Buffer } from 'node:buffer';
import type { Socket } from 'node:net';
import type { DataSource } from './data-source.js';

const NBDMAGIC = 0x4e42444d41474943n;
const IHAVEOPT = 0x49484156454f5054n;
const OPT_REPLY_MAGIC = 0x0003e889045565a9n;
const REQ_MAGIC = 0x25609513;
const SIMPLE_REPLY_MAGIC = 0x67446698;

const NBD_FLAG_FIXED_NEWSTYLE = 1;
const NBD_FLAG_NO_ZEROES = 2;

const NBD_OPT_ABORT = 2;
const NBD_OPT_INFO = 6;
const NBD_OPT_GO = 7;

const NBD_REP_ACK = 1;
const NBD_REP_INFO = 3;
const NBD_REP_ERR_UNSUP = 0x80000001;

const NBD_INFO_EXPORT = 0;
const NBD_FLAG_HAS_FLAGS = 1;
const NBD_FLAG_READ_ONLY = 2;

const NBD_CMD_READ = 0;
const NBD_CMD_DISC = 2;
const NBD_CMD_FLUSH = 3;

const EINVAL = 22;
const EIO = 5;

const MAX_IN_FLIGHT = 16;

/** Serial byte reader over a socket (single outstanding read at a time). */
function makeReader(socket: Socket): (n: number) => Promise<Buffer> {
  let chunks: Buffer[] = [];
  let buffered = 0;
  let want = 0;
  let resolveWant: ((b: Buffer) => void) | null = null;
  let rejectWant: ((e: Error) => void) | null = null;
  let ended = false;
  let errored: Error | null = null;

  function tryResolve() {
    if (resolveWant && buffered >= want) {
      const all = Buffer.concat(chunks);
      const out = all.subarray(0, want);
      const rest = all.subarray(want);
      chunks = rest.length ? [Buffer.from(rest)] : [];
      buffered = rest.length;
      const r = resolveWant;
      resolveWant = rejectWant = null;
      r(out);
    } else if (resolveWant && (ended || errored)) {
      const rej = rejectWant!;
      resolveWant = rejectWant = null;
      rej(errored ?? new Error('EOF'));
    }
  }

  socket.on('data', (d: Buffer) => {
    chunks.push(d);
    buffered += d.length;
    tryResolve();
  });
  socket.on('end', () => {
    ended = true;
    tryResolve();
  });
  socket.on('error', (e: Error) => {
    errored = e;
    tryResolve();
  });

  return (n: number) =>
    new Promise<Buffer>((resolve, reject) => {
      want = n;
      resolveWant = resolve;
      rejectWant = reject;
      tryResolve();
    });
}

/** Serialized reply-frame writer: one frame on the wire at a time. */
function makeWriter(socket: Socket) {
  let chain: Promise<void> = Promise.resolve();
  return (buf: Buffer): Promise<void> => {
    chain = chain.then(
      () =>
        new Promise<void>((res, rej) =>
          socket.write(buf, (e) => (e ? rej(e) : res())),
        ),
    );
    return chain;
  };
}

function simpleReply(error: number, handle: Buffer, data: Buffer | null): Buffer {
  const hdr = Buffer.alloc(16);
  hdr.writeUInt32BE(SIMPLE_REPLY_MAGIC, 0);
  hdr.writeUInt32BE(error >>> 0, 4);
  handle.copy(hdr, 8);
  return data ? Buffer.concat([hdr, data]) : hdr;
}

function optReply(opt: number, repType: number, payload: Buffer): Buffer {
  const fixed = Buffer.alloc(16);
  fixed.writeBigUInt64BE(OPT_REPLY_MAGIC, 0);
  fixed.writeUInt32BE(opt, 8);
  fixed.writeUInt32BE(repType >>> 0, 12);
  const lenBuf = Buffer.alloc(4);
  lenBuf.writeUInt32BE(payload.length, 0);
  return Buffer.concat([fixed, lenBuf, payload]);
}

/**
 * Serve one client on `socket` from `source`. Resolves on clean DISC/EOF.
 * Reads are dispatched concurrently (up to 16 in flight); replies are written
 * in completion order, keyed by the 8-byte handle.
 */
export async function serveNbd(
  socket: Socket,
  source: DataSource,
  opts: { size: number; onRead?: (offset: number, length: number) => void },
): Promise<void> {
  const read = makeReader(socket);
  const write = makeWriter(socket);
  const size = opts.size;

  /* Handshake */
  const hello = Buffer.alloc(18);
  hello.writeBigUInt64BE(NBDMAGIC, 0);
  hello.writeBigUInt64BE(IHAVEOPT, 8);
  hello.writeUInt16BE(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES, 16);
  await write(hello);
  await read(4); /* client flags */

  /* Option haggling until GO (enter transmission) or ABORT */
  for (;;) {
    const optHdr = await read(16);
    if (optHdr.readBigUInt64BE(0) !== IHAVEOPT) throw new Error('bad option magic');
    const opt = optHdr.readUInt32BE(8);
    const optLen = optHdr.readUInt32BE(12);
    if (optLen) await read(optLen);

    if (opt === NBD_OPT_GO || opt === NBD_OPT_INFO) {
      const info = Buffer.alloc(12);
      info.writeUInt16BE(NBD_INFO_EXPORT, 0);
      info.writeBigUInt64BE(BigInt(size), 2);
      info.writeUInt16BE(NBD_FLAG_HAS_FLAGS | NBD_FLAG_READ_ONLY, 10);
      await write(optReply(opt, NBD_REP_INFO, info));
      await write(optReply(opt, NBD_REP_ACK, Buffer.alloc(0)));
      if (opt === NBD_OPT_GO) break;
    } else if (opt === NBD_OPT_ABORT) {
      await write(optReply(opt, NBD_REP_ACK, Buffer.alloc(0)));
      socket.end();
      return;
    } else {
      await write(optReply(opt, NBD_REP_ERR_UNSUP, Buffer.alloc(0)));
    }
  }

  /* Transmission: dispatch reads concurrently, reply in completion order */
  let inFlight = 0;
  const pending = new Set<Promise<void>>();
  let slotWaiter: (() => void) | null = null;

  for (;;) {
    if (inFlight >= MAX_IN_FLIGHT) {
      await new Promise<void>((r) => (slotWaiter = r));
    }
    let hdr: Buffer;
    try {
      hdr = await read(28);
    } catch {
      break; /* EOF/disconnect */
    }
    if (hdr.readUInt32BE(0) !== REQ_MAGIC) throw new Error('bad request magic');
    const type = hdr.readUInt16BE(6);
    const handle = Buffer.from(hdr.subarray(8, 16)); /* copy: detach from reader buffer */
    const offset = hdr.readBigUInt64BE(16);
    const length = hdr.readUInt32BE(24);

    if (type === NBD_CMD_DISC) break;

    if (type === NBD_CMD_READ) {
      inFlight++;
      const job = (async () => {
        try {
          if (offset + BigInt(length) > BigInt(size)) {
            await write(simpleReply(EINVAL, handle, null));
            return;
          }
          const data = await source.read(Number(offset), length);
          /* A short read would desync the client (reply header claims success
           * but carries fewer bytes than requested). The upstream bounds check
           * + a fixed export size make this unreachable in normal operation;
           * treat it as an IO error rather than emit a malformed frame. */
          if (data.length !== length) throw new Error('short read');
          opts.onRead?.(Number(offset), length);
          await write(simpleReply(0, handle, data));
        } catch {
          /* The reply write itself can fail on a dead socket — swallow it so
           * the job never becomes an unhandled rejection. */
          try {
            await write(simpleReply(EIO, handle, null));
          } catch {
            /* socket gone; nothing more to do for this request */
          }
        } finally {
          inFlight--;
          if (slotWaiter) {
            const w: () => void = slotWaiter;
            slotWaiter = null;
            w();
          }
        }
      })();
      pending.add(job);
      job.finally(() => pending.delete(job));
    } else if (type === NBD_CMD_FLUSH) {
      try {
        await write(simpleReply(0, handle, null));
      } catch {
        break; /* socket died; stop reading and drain in-flight reads */
      }
    } else {
      try {
        await write(simpleReply(EINVAL, handle, null));
      } catch {
        break;
      }
    }
  }

  await Promise.allSettled([...pending]);
}
