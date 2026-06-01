import { Buffer } from 'node:buffer';

/** A read-only random-access byte source backing an NBD export. */
export interface DataSource {
  /** Total export size in bytes. */
  size(): Promise<number>;
  /** Read exactly `length` bytes at `offset`. Rejects on IO error. */
  read(offset: number, length: number): Promise<Buffer>;
  /** Release any held resources (fd, http agent, ...). */
  close(): Promise<void>;
}

export type DataSourceSpec =
  | { kind: 'file'; target: string }
  | { kind: 'blockdev'; target: string }
  | { kind: 'url'; target: string };

export async function createDataSource(spec: DataSourceSpec): Promise<DataSource> {
  switch (spec.kind) {
    case 'file': {
      const { FileSource } = await import('./sources/file.js');
      return FileSource.open(spec.target);
    }
    case 'blockdev': {
      const { BlockDeviceSource } = await import('./sources/blockdev.js');
      return BlockDeviceSource.open(spec.target);
    }
    case 'url': {
      const { HttpSource } = await import('./sources/http.js');
      return HttpSource.open(spec.target);
    }
    default: {
      /* Reachable only when an invalid `kind` is forced past the type system
       * (e.g. the CLI's `as DataSourceSpec` cast on unvalidated input). Fail
       * with a clear message instead of returning undefined. */
      const bad = spec as { kind?: unknown };
      throw new Error(`createDataSource: unknown source kind: ${String(bad.kind)}`);
    }
  }
}
