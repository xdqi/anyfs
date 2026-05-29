import { useEffect, useState } from 'react';
import type { DirEntry } from '@anyfs/core';
import { useAnyfsDisk } from './provider.js';

interface DirState {
    entries: DirEntry[] | null;
    loading: boolean;
    error: Error | null;
}

const cache = new WeakMap<object, Map<string, DirEntry[]>>();

export function useAnyfsDir(path: string | null): DirState {
    const { session, status } = useAnyfsDisk();
    const [state, setState] = useState<DirState>({
        entries: null,
        loading: false,
        error: null,
    });

    useEffect(() => {
        if (!session || status !== 'ready' || !path) return;
        const m = cache.get(session) ?? new Map<string, DirEntry[]>();
        cache.set(session, m);
        const hit = m.get(path);
        if (hit) {
            setState({ entries: hit, loading: false, error: null });
            return;
        }
        let cancelled = false;
        setState({ entries: null, loading: true, error: null });
        session.readdir(path).then(
            (entries) => {
                if (cancelled) return;
                m.set(path, entries);
                setState({ entries, loading: false, error: null });
            },
            (err) => {
                if (cancelled) return;
                setState({
                    entries: null,
                    loading: false,
                    error: err instanceof Error ? err : new Error(String(err)),
                });
            },
        );
        return () => {
            cancelled = true;
        };
    }, [session, status, path]);

    return state;
}
