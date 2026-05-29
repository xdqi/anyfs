import { useEffect, useState } from 'react';
import { useAnyfsDisk } from './provider.js';

export interface FileRange {
    offset: number;
    length: number;
}
interface FileState {
    data: Uint8Array | null;
    size: number | null;
    loading: boolean;
    error: Error | null;
}

/** Read a file (or range) lazily. Returns null `data` while loading. */
export function useAnyfsFile(path: string | null, range?: FileRange): FileState {
    const { session, status } = useAnyfsDisk();
    const [state, setState] = useState<FileState>({
        data: null,
        size: null,
        loading: false,
        error: null,
    });
    const off = range?.offset ?? 0;
    const len = range?.length ?? null;

    useEffect(() => {
        if (!session || status !== 'ready' || !path) return;
        let cancelled = false;
        setState({ data: null, size: null, loading: true, error: null });
        (async () => {
            try {
                const st = await session.stat(path);
                const readLen = len ?? Math.max(0, st.size - off);
                if (readLen === 0) {
                    if (!cancelled)
                        setState({
                            data: new Uint8Array(0),
                            size: st.size,
                            loading: false,
                            error: null,
                        });
                    return;
                }
                const fd = await session.openFd(path);
                try {
                    const data = await session.readFd(fd, off, readLen);
                    if (!cancelled)
                        setState({
                            data,
                            size: st.size,
                            loading: false,
                            error: null,
                        });
                } finally {
                    await session.closeFd(fd).catch(() => {});
                }
            } catch (err) {
                if (cancelled) return;
                setState({
                    data: null,
                    size: null,
                    loading: false,
                    error: err instanceof Error ? err : new Error(String(err)),
                });
            }
        })();
        return () => {
            cancelled = true;
        };
    }, [session, status, path, off, len]);

    return state;
}
