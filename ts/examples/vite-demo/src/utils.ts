import type { SessionSource } from '@anyfs/core';

/** Wipe the in-disk navigation hash. */
export function clearNavHash() {
    if (typeof window === 'undefined') return;
    if (!window.location.hash) return;
    window.history.replaceState(null, '', window.location.pathname + window.location.search);
}

/** User-facing label for a SessionSource. */
export function sourceName(s: SessionSource): string {
    if (s.kind === 'file') return s.file.name;
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
