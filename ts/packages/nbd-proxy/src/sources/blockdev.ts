import { Buffer } from 'node:buffer';
import { open, readFile, type FileHandle } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import type { DataSource } from '../data-source.js';

/**
 * Raw block device source. v1 is Linux-only: the kernel block layer serves
 * non-aligned preads, so no app-level alignment is needed. Size comes from
 * `drivelist` (optional dependency) or, as a fallback, /sys/block/<dev>/size.
 * macOS/Windows raw-device specifics (sector alignment, IOCTL size) are
 * deferred — open() throws a clear error there.
 */
export class BlockDeviceSource implements DataSource {
  private constructor(
    private fh: FileHandle,
    private bytes: number,
  ) {}

  static async open(devPath: string): Promise<BlockDeviceSource> {
    if (os.platform() !== 'linux') {
      throw new Error(
        `BlockDeviceSource: v1 is Linux-only (got ${os.platform()}); ` +
          `macOS/Windows raw-device support is not yet implemented`,
      );
    }
    const bytes = await BlockDeviceSource.sizeOf(devPath);
    const fh = await open(devPath, 'r');
    return new BlockDeviceSource(fh, bytes);
  }

  private static async sizeOf(devPath: string): Promise<number> {
    /* Prefer drivelist (optional dep): enumerate and match the device.
     * Use a variable-based import to avoid TS2307 in declaration emit
     * (drivelist has no type declarations). */
    try {
      const modName = 'drivelist';
      const drivelist = (await import(modName)) as {
        list: () => Promise<Array<{ device: string; size: number }>>;
      };
      const drives = await drivelist.list();
      const hit = drives.find((d) => d.device === devPath);
      if (hit && hit.size > 0) return hit.size;
    } catch {
      /* drivelist absent or failed — fall through to /sys/block. */
    }
    /* Fallback: /sys/block/<name>/size is in 512-byte sectors. */
    const name = path.basename(devPath);
    const sysSize = path.join('/sys/block', name, 'size');
    const sectors = Number((await readFile(sysSize, 'utf8')).trim());
    if (!Number.isFinite(sectors) || sectors <= 0) {
      throw new Error(`BlockDeviceSource: could not determine size of ${devPath}`);
    }
    return sectors * 512;
  }

  async size(): Promise<number> {
    return this.bytes;
  }

  async read(offset: number, length: number): Promise<Buffer> {
    const buf = Buffer.alloc(length);
    let got = 0;
    while (got < length) {
      const { bytesRead } = await this.fh.read(buf, got, length - got, offset + got);
      if (bytesRead === 0) break;
      got += bytesRead;
    }
    return got === length ? buf : buf.subarray(0, got);
  }

  async close(): Promise<void> {
    await this.fh.close();
  }
}
