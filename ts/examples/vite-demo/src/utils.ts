import { applyUrlProxy, getAnyfsNative, getUrlProxyPrefix } from '@anyfs/core';
import type { SessionSource } from '@anyfs/core';

/** Wipe the in-disk navigation hash. */
export function clearNavHash() {
    if (typeof window === 'undefined') return;
    if (!window.location.hash) return;
    window.history.replaceState(null, '', window.location.pathname + window.location.search);
}

/** User-facing label for a SessionSource. */
export function sourceName(s: SessionSource): string {
    if (s.kind === 'blob') return (s.blob as File).name;
    if (s.kind === 'path') {
        if (s.name) return s.name;
        const parts = s.path.split(/[\\/]/).filter(Boolean);
        return parts[parts.length - 1] || s.path;
    }
    try {
        const u = new URL(s.url, window.location.href);
        const last = u.pathname.split('/').filter(Boolean).pop();
        if (last) return decodeURIComponent(last);
        return u.host || s.url;
    } catch {
        return s.url;
    }
}

/**
 * Convert a browser File into a SessionSource. Under Electron *with the native
 * backend active*, if the `electronFile.pathFor` bridge is available the File is
 * translated to an absolute host path and `{kind:'path'}` is returned (the
 * native LKL kernel mounts the host path directly). Otherwise — in a plain
 * browser, OR in Electron when native is disabled (wasm fallback) — the file
 * stays as `{kind:'blob'}` so the WORKERFS/wasm path can mount it. We gate on
 * `getAnyfsNative()` rather than `electronFile.pathFor` alone because the
 * `electronFile` bridge is exposed in BOTH modes, but only the native backend
 * can consume a host path; the wasm backend rejects `{kind:'path'}`.
 */
export async function fileToSource(file: File): Promise<SessionSource> {
    const ef = (window as any).electronFile as
        | { pathFor?: (f: File) => Promise<string> }
        | undefined;
    if (ef?.pathFor && getAnyfsNative()) {
        try {
            const p = await ef.pathFor(file);
            const name = sourceName({ kind: 'path', path: p });
            return { kind: 'path', path: p, name };
        } catch {
            // pathFor failed — fall through to blob
        }
    }
    return { kind: 'blob', blob: file };
}

/** Format a Unix timestamp (seconds) as human-readable. */
export function formatTs(ts: number): string {
    if (!ts) return '—';
    return new Date(ts * 1000).toISOString().replace('T', ' ').slice(0, 19);
}

/** Human-readable partition type label. */
export function ptLabel(pt: string): string {
    if (!pt) return 'unknown';
    switch (pt) {
        case 'dos':
            return 'MBR';
        case 'gpt':
            return 'GPT';
        default:
            return pt.toUpperCase();
    }
}

/** Probe a URL via async HEAD on the main thread. Returns the size on
 *  success; throws a user-readable Error on failure.  */
export async function probeUrlAhead(url: string): Promise<number> {
    const fetchUrl = applyUrlProxy(url);
    let resp: Response;
    try {
        resp = await fetch(fetchUrl, { method: 'HEAD', cache: 'no-store' });
    } catch (e) {
        const msg = getUrlProxyPrefix()
            ? `Couldn't reach the URL — DNS, TLS, or the host is down. ` +
              `See the console for the real error.`
            : `Couldn't reach the URL — usually CORS (the server didn't send ` +
              `Access-Control-Allow-Origin), or the host is down. ` +
              `Browser console has the real error.`;
        throw new Error(msg, { cause: e });
    }
    if (!resp.ok) {
        throw new Error(`Server returned HTTP ${resp.status} ${resp.statusText}.`);
    }
    const cl = resp.headers.get('Content-Length');
    if (!cl) {
        throw new Error(
            `Server didn't return a Content-Length header on HEAD. ` +
                `URLFS needs to know the file size up front to scan partitions.`,
        );
    }
    const size = Number.parseInt(cl, 10);
    if (!Number.isFinite(size) || size <= 0) {
        throw new Error(`Server returned an invalid Content-Length: ${cl}.`);
    }
    const ar = (resp.headers.get('Accept-Ranges') ?? '').toLowerCase();
    if (!ar.includes('bytes')) {
        let probe: Response;
        try {
            probe = await fetch(fetchUrl, {
                method: 'GET',
                headers: { Range: 'bytes=0-0' },
                cache: 'no-store',
            });
        } catch (e) {
            throw new Error(`Range probe failed: ${(e as Error).message}`, { cause: e });
        }
        if (probe.status !== 206) {
            throw new Error(
                `Server doesn't support Range requests ` +
                    `(got HTTP ${probe.status} for a Range probe). ` +
                    `URLFS needs partial reads, not a full download.`,
            );
        }
    }
    return size;
}
