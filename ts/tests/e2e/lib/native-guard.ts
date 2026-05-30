import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { ELECTRON_DEMO_DIR, TS_ROOT } from './paths';

/** electron-native MUST have an ABI-matching addon, or FAIL (not skip) — ABI
 *  drift SIGSEGVs at require() and a silent skip would hide a broken native path. */
export function assertNativeAddonPresent(): void {
    const candidates = [
        resolve(ELECTRON_DEMO_DIR, 'resources/native/anyfs_native.node'),
        resolve(TS_ROOT, 'packages/anyfs-native/build/Release/anyfs_native.node'),
    ];
    if (!candidates.some(existsSync)) {
        throw new Error(
            'electron-native: no anyfs_native.node found. Build it (cd ts/packages/anyfs-native && npx node-gyp build) before running.',
        );
    }
}
