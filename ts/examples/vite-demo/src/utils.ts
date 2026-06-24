import { applyUrlProxy, getAnyfsNative, getUrlProxyPrefix } from '@anyfs/core';
import type { SessionSource } from '@anyfs/core';

/** Wipe the in-disk navigation hash. */
export function clearNavHash() {
    if (typeof window === 'undefined') return;
    if (!window.location.hash) return;
    window.history.replaceState(null, '', window.location.pathname + window.location.search);
}

/** A stable identity string for a source, used as a React `key` so the disk
 *  view remounts fresh on every switch (drops the previous disk's cached
 *  partition list / mount path / size instead of flashing them — findings
 *  F16-10/11/23). Distinct disks map to distinct keys; the same disk reopened
 *  maps to the same key (a harmless no-op remount). */
export function sourceKey(s: SessionSource | null): string {
    if (!s) return 'none';
    if (s.kind === 'url') return `url:${s.url}`;
    if (s.kind === 'path') return `path:${s.path}`;
    const b = s.blob as File;
    return `blob:${b.name ?? ''}:${b.size}:${b.lastModified ?? 0}`;
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

/** Total length from `Content-Range: bytes 0-0/12345`; NaN if absent/unknown. */
function totalFromContentRange(header: string | null): number {
    const m = (header ?? '').match(/\/\s*(\d+)\s*$/);
    return m ? Number.parseInt(m[1], 10) : Number.NaN;
}

/** Probe a URL ahead of mount. Tries HEAD for the size + range support, then
 *  falls back to a `Range: bytes=0-0` GET — which both confirms partial reads
 *  and reads the total from Content-Range. That fallback is what lets URLFS load
 *  Range-capable servers that reject HEAD (400/405) or omit Content-Length
 *  (e.g. the "Everything" HTTP server: HEAD → 400, ranged GET → 206). Returns
 *  the size; throws a user-readable Error on failure. */
export async function probeUrlAhead(url: string): Promise<number> {
    const fetchUrl = applyUrlProxy(url);
    const corsMsg = getUrlProxyPrefix()
        ? `Couldn't reach the URL — DNS, TLS, or the host is down. ` +
          `See the console for the real error.`
        : `Couldn't reach the URL — usually CORS (the server didn't send ` +
          `Access-Control-Allow-Origin), or the host is down. ` +
          `Browser console has the real error.`;

    // 1) HEAD: cheapest path to size + range support. A network/CORS failure
    //    here is NOT fatal yet — some servers/proxies reject HEAD but honor a
    //    ranged GET, so we try that before giving up.
    let size = Number.NaN;
    let rangeOk = false;
    let headStatus = 0;
    let headNetworkError: unknown = null;
    try {
        const resp = await fetch(fetchUrl, { method: 'HEAD', cache: 'no-store' });
        headStatus = resp.status;
        if (resp.ok) {
            const cl = resp.headers.get('Content-Length');
            if (cl) size = Number.parseInt(cl, 10);
            rangeOk = (resp.headers.get('Accept-Ranges') ?? '').toLowerCase().includes('bytes');
        }
    } catch (e) {
        headNetworkError = e;
    }

    // 2) Range GET fallback when HEAD didn't give a usable size or range support.
    if (!Number.isFinite(size) || size <= 0 || !rangeOk) {
        let probe: Response;
        try {
            probe = await fetch(fetchUrl, {
                method: 'GET',
                headers: { Range: 'bytes=0-0' },
                cache: 'no-store',
            });
        } catch (e) {
            // Both HEAD and the Range GET failed at the network layer → CORS/host-down.
            throw new Error(corsMsg, { cause: headNetworkError ?? e });
        }
        if (probe.status === 206) {
            rangeOk = true;
            const total = totalFromContentRange(probe.headers.get('Content-Range'));
            if (Number.isFinite(total) && total > 0) size = total;
        } else if (probe.status === 200) {
            throw new Error(
                `Server ignored the Range request (HTTP 200), so partial reads aren't ` +
                    `possible. URLFS needs Range, not a full download.`,
            );
        } else {
            throw new Error(
                `Server returned HTTP ${probe.status} for a Range probe` +
                    (headStatus ? ` (HEAD was HTTP ${headStatus}).` : `.`),
            );
        }
    }

    if (!Number.isFinite(size) || size <= 0) {
        throw new Error(
            `Couldn't determine the file size — no Content-Length on HEAD and no total ` +
                `in the Range response's Content-Range header.`,
        );
    }
    return size;
}
