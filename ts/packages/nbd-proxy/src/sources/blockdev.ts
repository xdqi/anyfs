import type { DataSource } from '../data-source.js';

export class BlockDeviceSource implements DataSource {
  static async open(_path: string): Promise<BlockDeviceSource> {
    throw new Error('not implemented yet');
  }
  async size(): Promise<number> {
    throw new Error('not implemented yet');
  }
  async read(_offset: number, _length: number): Promise<Buffer> {
    throw new Error('not implemented yet');
  }
  async close(): Promise<void> {
    throw new Error('not implemented yet');
  }
}
