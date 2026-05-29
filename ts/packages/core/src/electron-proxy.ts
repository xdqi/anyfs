/**
 * URL proxy contract — host-agnostic.
 *
 * Some shells (Electron, Tauri, future N-API hosts) can fetch http(s)
 * URLs without the browser's same-origin policy. They opt in by setting
 * `globalThis.__anyfs = { urlProxyPrefix: '<scheme>://...?u=' }` before
 * any URL is read; the prefix gets `encodeURIComponent(targetUrl)`
 * appended to form the actual fetch URL.
 *
 * Renderer side: preload exposes the field via `contextBridge`.
 * Worker side: the renderer copies the prefix into the boot message and
 * the worker writes it onto its own `globalThis.__anyfs`, so this same
 * helper resolves correctly from both contexts.
 *
 * Plain browsers (no shell) leave the global unset and URLs pass
 * through verbatim.
 */

interface AnyfsHostBridge {
    urlProxyPrefix?: string;
}

function host(): AnyfsHostBridge | undefined {
    try {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const g = (globalThis as any).__anyfs;
        return g && typeof g === 'object' ? (g as AnyfsHostBridge) : undefined;
    } catch {
        return undefined;
    }
}

export function getUrlProxyPrefix(): string | undefined {
    const p = host()?.urlProxyPrefix;
    return typeof p === 'string' && p.length > 0 ? p : undefined;
}

/** Rewrite an http(s) URL through the configured proxy prefix, if any.
 *  Non-http URLs, unconfigured environments, and loopback addresses pass
 *  through unchanged — loopback servers are under developer control and
 *  ship their own CORS headers. */
export function applyUrlProxy(url: string): string {
    const prefix = getUrlProxyPrefix();
    if (!prefix) return url;
    if (!/^https?:\/\//i.test(url)) return url;
    // Don't proxy loopback — CORS headers are under our control.
    if (/^https?:\/\/(localhost|127\.\d+\.\d+\.\d+)(:\d+)?(\/|$)/i.test(url)) return url;
    return prefix + encodeURIComponent(url);
}

/** Worker-side setter: copy the prefix received via boot message onto
 *  the worker's globalThis so applyUrlProxy() resolves identically here.
 *  No-op if `prefix` is falsy. */
export function setUrlProxyPrefix(prefix: string | undefined): void {
    if (!prefix) return;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const g = globalThis as any;
    const existing = (g.__anyfs ?? {}) as AnyfsHostBridge;
    existing.urlProxyPrefix = prefix;
    g.__anyfs = existing;
}
