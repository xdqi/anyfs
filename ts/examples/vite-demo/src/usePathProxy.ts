import { useMemo } from 'react';
import type { SessionSource } from '@anyfs/core';
import { getUrlProxyPrefix } from '@anyfs/core';
import { sourceName } from './utils';

/**
 * Make a {kind:'path'} source mountable by the WASM backend (Electron with the
 * native addon disabled/unavailable).
 *
 * The browser/wasm backend can't open a host path directly, and reading the
 * whole file into a Blob would blow memory on multi-GiB images. Instead we
 * rewrite the path to a URL the wasm worker CAN read: `anyfs-url://proxy/?u=
 * file://<abs path>`. The worker's URLFS range-reads that via XHR; the
 * anyfs-url:// scheme is handled in the Electron main process (handleAnyfsUrlRequest)
 * which streams the file with `net.fetch('file://…')` + Range — the exact same
 * mechanism that already serves the bundled /disks/ images. No proxy worker, no
 * lifecycle to manage: this is a pure synchronous string rewrite.
 *
 * Passthrough (returns `source` verbatim) whenever `enabled` is false, the
 * source is absent, or it isn't a path — protecting native mode (NativeSession
 * keeps using attachPath), the web backend, and electron-wasm blob/url opens.
 *
 * The `anyfs-url://` prefix comes from the host bridge (`getUrlProxyPrefix()`,
 * set by preload to `anyfs-url://proxy/?u=`). We wrap the file URL ourselves
 * rather than going through applyUrlProxy() because the wasm worker re-applies
 * applyUrlProxy() on every request and we want the final, already-wrapped URL
 * to pass through there untouched (it's non-http, so it does).
 */
export interface UsePathProxyResult {
    /** The source to hand to <AnyfsProvider>: the original for non-path / native
     *  cases, a rewritten {kind:'url'} for a proxied path, or null on error. */
    source: SessionSource | null;
    /** Set when a path source can't be proxied (no host url-proxy bridge).
     *  The caller should surface this and keep the picker visible. */
    error: Error | null;
}

/** file:// URL for an absolute host path, browser-safe (no Node `pathToFileURL`).
 *  Normalises Windows backslashes and a drive-letter root to a leading slash,
 *  and percent-encodes each segment while preserving the separators. */
function toFileUrl(absPath: string): string {
    let p = absPath.replace(/\\/g, '/');
    if (!p.startsWith('/')) p = '/' + p; // C:/… → /C:/…
    const encoded = p
        .split('/')
        .map((seg) => encodeURIComponent(seg))
        .join('/');
    return 'file://' + encoded;
}

export function usePathProxy(
    source: SessionSource | null,
    enabled: boolean,
): UsePathProxyResult {
    return useMemo<UsePathProxyResult>(() => {
        // Passthrough: native mode, no source, or a non-path kind.
        if (!enabled || !source || source.kind !== 'path') {
            return { source, error: null };
        }

        const prefix = getUrlProxyPrefix();
        if (!prefix) {
            return {
                source: null,
                error: new Error(
                    "Can't open a local path in this mode — the Electron url proxy is " +
                        'unavailable. Re-open the file via drag-drop or “Open file…” so it ' +
                        'loads as a blob, or re-enable the native engine in Settings.',
                ),
            };
        }

        const url = prefix + encodeURIComponent(toFileUrl(source.path));
        return { source: { kind: 'url', url, name: sourceName(source) }, error: null };
    }, [source, enabled]);
}
