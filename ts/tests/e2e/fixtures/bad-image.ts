import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/** A 1 MiB file of zeros — no partition table, no filesystem. */
export function makeBadImage(): string {
    const dir = mkdtempSync(join(tmpdir(), 'anyfs-bad-'));
    const file = join(dir, 'corrupt.img');
    writeFileSync(file, Buffer.alloc(1 << 20, 0));
    return file;
}
