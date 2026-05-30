import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { closeSync, existsSync, openSync, readSync, statSync } from 'node:fs';
import { E2E_DIR } from '../lib/paths';
import { FIXTURES, type Fixture } from './manifest';

/** Stream the file through the hash in fixed-size chunks so we never load a
 *  600+ MiB iso fully into memory. Kept synchronous (readSync loop) so callers
 *  can run ensureFixture at module top-level. */
function sha256(file: string): string {
    const hash = createHash('sha256');
    const fd = openSync(file, 'r');
    try {
        const buf = Buffer.allocUnsafe(1 << 20); // 1 MiB
        let read: number;
        while ((read = readSync(fd, buf, 0, buf.length, null)) > 0) {
            hash.update(buf.subarray(0, read));
        }
    } finally {
        closeSync(fd);
    }
    return hash.digest('hex');
}

/** Build (generated) or fetch (downloaded) a fixture if missing, verify guards. */
export function ensureFixture(name: keyof typeof FIXTURES): Fixture {
    const fx = FIXTURES[name];
    if (!existsSync(fx.file)) {
        const script = fx.source === 'generated' ? 'fixtures/generate.mjs' : 'fixtures/fetch.mjs';
        execFileSync('node', [script], { cwd: E2E_DIR, stdio: 'inherit' });
    }
    if (!existsSync(fx.file)) throw new Error(`fixture ${name} still missing after build`);
    if (fx.source === 'downloaded') {
        if (fx.expectedSize && statSync(fx.file).size !== fx.expectedSize) {
            throw new Error(`fixture ${name} size drift: remote changed?`);
        }
        if (fx.sha256 && sha256(fx.file) !== fx.sha256) {
            throw new Error(`fixture ${name} sha256 drift: remote changed?`);
        }
    }
    return fx;
}
