import { Buffer } from 'node:buffer';
import { open, type FileHandle } from 'node:fs/promises';
import type { DataSource } from '../data-source.js';

export class FileSource implements DataSource {
  private constructor(
    private fh: FileHandle,
    private bytes: number,
  ) {}

  static async open(path: string): Promise<FileSource> {
    const fh = await open(path, 'r');
    const st = await fh.stat();
    return new FileSource(fh, st.size);
  }

  async size(): Promise<number> {
    return this.bytes;
  }

  async read(offset: number, length: number): Promise<Buffer> {
    const buf = Buffer.alloc(length);
    let got = 0;
    while (got < length) {
      const { bytesRead } = await this.fh.read(buf, got, length - got, offset + got);
      if (bytesRead === 0) break; // EOF (e.g. read past end)
      got += bytesRead;
    }
    return got === length ? buf : buf.subarray(0, got);
  }

  async close(): Promise<void> {
    await this.fh.close();
  }
}
