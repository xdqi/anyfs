import { Buffer } from 'node:buffer';
import type { DataSource } from '../data-source.js';

/**
 * HTTP(S) Range-backed source using Node's global fetch. undici's global
 * dispatcher pools connections per origin, so range reads reuse upstream
 * connections (no per-request reconnect). H2 multiplexing is deferred; v1
 * relies on HTTP/1.1 keep-alive via undici's default pool.
 */
export class HttpSource implements DataSource {
  private constructor(
    private url: string,
    private bytes: number,
  ) {}

  static async open(url: string): Promise<HttpSource> {
    const head = await fetch(url, { method: 'HEAD', redirect: 'follow' });
    if (!head.ok) throw new Error(`HttpSource: HEAD ${url} -> ${head.status}`);
    const len = Number(head.headers.get('content-length') ?? '0');
    if (!len) throw new Error(`HttpSource: missing content-length for ${url}`);
    if ((head.headers.get('accept-ranges') ?? '').toLowerCase() !== 'bytes') {
      throw new Error(`HttpSource: server lacks Accept-Ranges: bytes for ${url}`);
    }
    return new HttpSource(head.url || url, len);
  }

  async size(): Promise<number> {
    return this.bytes;
  }

  async read(offset: number, length: number): Promise<Buffer> {
    const end = Math.min(offset + length, this.bytes) - 1;
    const res = await fetch(this.url, {
      headers: { Range: `bytes=${offset}-${end}` },
      redirect: 'follow',
    });
    if (res.status !== 206 && res.status !== 200) {
      throw new Error(`HttpSource: range ${offset}-${end} -> ${res.status}`);
    }
    let buf = Buffer.from(await res.arrayBuffer());
    if (res.status === 200 && buf.length > end - offset + 1) {
      buf = buf.subarray(offset, end + 1);
    }
    return buf;
  }

  async close(): Promise<void> {}
}
