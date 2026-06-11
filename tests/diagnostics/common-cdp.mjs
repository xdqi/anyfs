/*
 * Minimal CDP (Chrome DevTools Protocol) client for headless testing.
 *
 * Connects to a browser target via WebSocket, sends commands, and collects
 * console messages / evaluation results. No external dependencies.
 */
import { randomBytes } from 'node:crypto';
import { request as httpRequest } from 'node:http';

// ── WebSocket framing (RFC 6455, minimal) ───────────────────────────────
function wsKey() {
  return randomBytes(16).toString('base64');
}

/** Encode a masked text frame (opcode 0x1). Client→server frames MUST be masked per RFC 6455. */
function encodeFrame(payload) {
  const buf = Buffer.from(payload);
  const maskKey = randomBytes(4);
  let hdrLen, frame;
  if (buf.length < 126) {
    hdrLen = 2;
    frame = Buffer.alloc(hdrLen + 4 + buf.length);
    frame[0] = 0x81;
    frame[1] = 0x80 | buf.length;
  } else if (buf.length < 65536) {
    hdrLen = 4;
    frame = Buffer.alloc(hdrLen + 4 + buf.length);
    frame[0] = 0x81;
    frame[1] = 0x80 | 126;
    frame.writeUInt16BE(buf.length, 2);
  } else {
    hdrLen = 10;
    frame = Buffer.alloc(hdrLen + 4 + buf.length);
    frame[0] = 0x81;
    frame[1] = 0x80 | 127;
    frame.writeBigUInt64BE(BigInt(buf.length), 2);
  }
  maskKey.copy(frame, hdrLen);
  for (let i = 0; i < buf.length; i++) frame[hdrLen + 4 + i] = buf[i] ^ maskKey[i & 3];
  return frame;
}

export class CDPClient {
  /**
   * @param {string} wsUrl  e.g. ws://127.0.0.1:9229/devtools/page/ABC
   */
  constructor(wsUrl) {
    this.url = new URL(wsUrl);
    this._id = 0;
    this._pending = new Map();
    /** @type {Array<{method:string,params:unknown}>} */
    this.events = [];
    this._buf = Buffer.alloc(0);
    this._closed = false;
    this._sock = null;
  }

  async connect() {
    const { hostname, port, pathname, search } = this.url;
    const key = wsKey();
    const req = httpRequest({
      hostname,
      port: port || 80,
      path: pathname + search,
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': key,
        'Sec-WebSocket-Version': '13',
      },
    });
    req.end();

    // Use the 'upgrade' event — the proper Node.js way to get the raw socket
    // after a 101 Switching Protocols response.
    const sock = await new Promise((resolve, reject) => {
      req.on('upgrade', (_res, socket) => resolve(socket));
      req.on('response', (res) => {
        // Drain response body, then reject
        res.resume();
        reject(new Error(`WS handshake failed: ${res.statusCode}`));
      });
      req.on('error', reject);
    });
    this._sock = sock;

    sock.on('data', (chunk) => {
      this._buf = Buffer.concat([this._buf, chunk]);
      this._drainFrames();
    });
    sock.on('close', () => { this._closed = true; });
    sock.on('error', () => { this._closed = true; });
  }

  _drainFrames() {
    let offset = 0;
    while (offset < this._buf.length) {
      if (this._buf.length - offset < 2) break;
      const opcode = this._buf[offset] & 0x0f;
      const masked = !!(this._buf[offset + 1] & 0x80);
      const len1 = this._buf[offset + 1] & 0x7f;
      let payloadLen = len1;
      let hdrLen = 2;
      if (len1 === 126) {
        if (this._buf.length - offset < 4) break;
        payloadLen = this._buf.readUInt16BE(offset + 2);
        hdrLen = 4;
      } else if (len1 === 127) {
        if (this._buf.length - offset < 10) break;
        payloadLen = Number(this._buf.readBigUInt64BE(offset + 2));
        hdrLen = 10;
      }
      // Server→client frames have a 4-byte mask key (RFC 6455 §5.3)
      const maskLen = masked ? 4 : 0;
      if (this._buf.length - offset < hdrLen + maskLen + payloadLen) break;

      let payload = this._buf.subarray(offset + hdrLen + maskLen, offset + hdrLen + maskLen + payloadLen);
      if (masked) {
        const maskKey = this._buf.subarray(offset + hdrLen, offset + hdrLen + 4);
        payload = Buffer.allocUnsafe(payloadLen);
        for (let i = 0; i < payloadLen; i++) payload[i] = this._buf[offset + hdrLen + 4 + i] ^ maskKey[i & 3];
      }
      offset += hdrLen + maskLen + payloadLen;

      if (opcode === 0x8) {
        this._closed = true;
        this._buf = this._buf.subarray(offset);
        return;
      }
      if (opcode === 0x9) {
        // ping -> pong
        const pong = Buffer.alloc(2 + payloadLen);
        pong[0] = 0x8a;
        pong[1] = payloadLen;
        if (payloadLen) payload.copy(pong, 2);
        this._sock.write(pong);
        continue;
      }
      if (opcode === 0x1) {
        const msg = JSON.parse(payload.toString());
        if (msg.id && this._pending.has(msg.id)) {
          const { resolve } = this._pending.get(msg.id);
          this._pending.delete(msg.id);
          resolve(msg);
        } else {
          this.events.push(msg);
        }
      }
    }
    this._buf = this._buf.subarray(offset);
  }

  /** Send a command and wait for the response. */
  async send(method, params = {}) {
    const id = ++this._id;
    const frame = encodeFrame(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject });
      this._sock.write(frame);
      setTimeout(() => {
        if (this._pending.has(id)) {
          this._pending.delete(id);
          reject(new Error(`CDP command ${method} timed out`));
        }
      }, 30000);
    });
  }

  /**
   * Evaluate a JS expression in the page and return the result value.
   */
  async evaluate(expression) {
    const resp = await this.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (resp.error) throw new Error(`CDP eval error: ${JSON.stringify(resp.error)}`);
    if (resp.result.exceptionDetails) {
      throw new Error(
        `JS exception: ${JSON.stringify(resp.result.exceptionDetails.exception?.description || resp.result.exceptionDetails.text)}`,
      );
    }
    return resp.result.result?.value;
  }

  /** Wait for a console message whose text includes `substr`. */
  async waitForConsole(substr, timeoutMs = 30000) {
    await this.send('Runtime.enable');
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      for (let i = 0; i < this.events.length; i++) {
        const e = this.events[i];
        if (
          e.method === 'Runtime.consoleAPICalled' &&
          e.params?.args?.[0]?.value?.includes(substr)
        ) {
          this.events.splice(i, 1);
          return e.params;
        }
      }
      await sleep(500);
    }
    throw new Error(`Timed out waiting for console message: "${substr}"`);
  }

  /** Get the CDP target list from a debugging endpoint. */
  static async listTargets(host, port) {
    const raw = await httpGet(`http://${host}:${port}/json/list`);
    return JSON.parse(raw);
  }

  /** Get browser version info. */
  static async getVersion(host, port) {
    const raw = await httpGet(`http://${host}:${port}/json/version`);
    return JSON.parse(raw);
  }
}

// ── helpers ──────────────────────────────────────────────────────────────

function httpGet(url) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const req = httpRequest(
      {
        hostname: u.hostname,
        port: u.port || 80,
        path: u.pathname + u.search,
        method: 'GET',
      },
      (res) => {
        let data = '';
        res.on('data', (c) => (data += c));
        res.on('end', () => resolve(data));
      },
    );
    req.on('error', reject);
    req.end();
  });
}

export function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}
