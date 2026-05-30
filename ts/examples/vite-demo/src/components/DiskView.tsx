import { useCallback, useEffect, useState } from 'react';
import { applyUrlProxy } from '@anyfs/core';
import { useAnyfsDisk } from '@anyfs/react';
import { AnyfsFileBrowser } from '@anyfs/trees';
import type { NativeSession, SessionPartInfo, SessionMeta, SessionSource } from '@anyfs/core';
import { SupportedFormats } from './SupportedFormats';
import { DiskSummary } from './DiskSummary';
import { DownloadingFileTree } from './DownloadingFileTree';

export function DiskView({
    source,
    selectedPart,
    setSelectedPart,
}: {
    source: SessionSource;
    selectedPart: number | null;
    setSelectedPart: (n: number | null) => void;
}) {
    const { session, mountPath, status, step, error } = useAnyfsDisk();
    const [parts, setParts] = useState<SessionPartInfo[] | null>(null);
    const [meta, setMeta] = useState<SessionMeta | null>(null);
    const [onDiskSize, setOnDiskSize] = useState<number | null>(null);
    const [manualMount, setManualMount] = useState<string | null>(null);
    const [mountError, setMountError] = useState<string | null>(null);

    // When App steps us back out of a partition (selectedPart → null) we
    // need to drop the cached mount path too, otherwise re-picking the same
    // partition shows the previous mount instead of remounting fresh.
    useEffect(() => {
        if (selectedPart === null) {
            setManualMount(null);
            setMountError(null);
        }
    }, [selectedPart]);

    useEffect(() => {
        if (!session || status !== 'ready') return;
        session
            .listParts()
            .then(setParts)
            .catch(() => setParts([]));
        session
            .meta()
            .then(setMeta)
            .catch(() => setMeta(null));
    }, [session, status]);

    // Resolve "on-disk" (raw container) size: File.size for local, a HEAD's
    // Content-Length for URL. Differs from logical_size for qcow2/etc.
    // For native host paths we'd need an IPC stat — not wired yet; the
    // disk's logical_size is shown alone in that case.
    useEffect(() => {
        if (source.kind === 'blob') {
            setOnDiskSize(source.blob.size);
            return;
        }
        if (source.kind === 'path') {
            // No stat IPC for native paths yet; fall back to logical_size.
            return;
        }
        let cancelled = false;
        fetch(applyUrlProxy(source.url), { method: 'HEAD', cache: 'no-store' })
            .then((r) => {
                const cl = r.headers.get('Content-Length');
                const n = cl ? Number.parseInt(cl, 10) : NaN;
                if (!cancelled && Number.isFinite(n) && n > 0) setOnDiskSize(n);
            })
            .catch(() => {
                /* best effort */
            });
        return () => {
            cancelled = true;
        };
    }, [source]);

    useEffect(() => {
        if (!session || selectedPart === null) return;
        let cancelled = false;
        setMountError(null);
        session.enter(selectedPart).then(
            (mp) => {
                if (!cancelled) setManualMount(mp);
            },
            (err: unknown) => {
                if (cancelled) return;
                const msg = err instanceof Error ? err.message : String(err);
                setMountError(msg);
            },
        );
        return () => {
            cancelled = true;
        };
    }, [session, selectedPart]);

    if (
        status === 'booting' ||
        status === 'booted' ||
        status === 'attaching' ||
        status === 'mounting'
    ) {
        return (
            <div className="flex-1 flex items-center justify-center text-base text-zinc-600 dark:text-zinc-400">
                {step ?? 'Working…'}
            </div>
        );
    }
    if (status === 'error')
        return (
            <div className="flex-1 flex items-center justify-center text-base text-red-500 dark:text-red-400">
                Error: {error?.message}
            </div>
        );
    if (status !== 'ready') return null;

    if (parts === null)
        return (
            <div className="flex-1 flex items-center justify-center text-base text-zinc-600 dark:text-zinc-400">
                Reading partition table…
            </div>
        );
    if (selectedPart === null) {
        return (
            <section className="flex-1 flex flex-col items-center justify-center p-6">
                <h2 className="text-2xl font-semibold mb-2 text-zinc-900 dark:text-zinc-100">
                    choose a partition
                </h2>
                <DiskSummary source={source} meta={meta} onDiskSize={onDiskSize} />
                <ul className="space-y-1.5 w-full max-w-lg mt-4">
                    <li key="whole">
                        <button
                            className="w-full text-left text-base px-4 py-3 rounded-lg bg-zinc-100 hover:bg-zinc-200 dark:bg-zinc-800 dark:hover:bg-zinc-700"
                            onClick={() => setSelectedPart(0)}
                        >
                            <span className="font-mono text-emerald-600 dark:text-emerald-400">
                                #0
                            </span>
                            {'  '}
                            <span className="text-zinc-600 dark:text-zinc-400">
                                {meta
                                    ? `${(meta.logical_size / (1 << 20)).toFixed(1)} MiB`
                                    : 'Whole disk'}
                            </span>
                            <span className="ml-2 text-zinc-800 dark:text-zinc-300">
                                Whole disk
                            </span>
                        </button>
                    </li>
                    {parts.map((p) => (
                        <li key={p.slot_id}>
                            <button
                                className="w-full text-left text-base px-4 py-3 rounded-lg bg-zinc-100 hover:bg-zinc-200 dark:bg-zinc-800 dark:hover:bg-zinc-700"
                                onClick={() => setSelectedPart(p.index)}
                            >
                                <span className="font-mono text-emerald-600 dark:text-emerald-400">
                                    #{p.index}
                                </span>
                                {'  '}
                                <span className="text-zinc-600 dark:text-zinc-400">
                                    {(p.size / (1 << 20)).toFixed(1)} MiB
                                </span>
                                {p.label && (
                                    <span className="ml-2 text-zinc-800 dark:text-zinc-300">
                                        {p.label}
                                    </span>
                                )}
                                {p.fstype && (
                                    <span className="ml-2 text-sm text-zinc-500">{p.fstype}</span>
                                )}
                            </button>
                        </li>
                    ))}
                </ul>
            </section>
        );
    }

    return (
        <section className="flex-1 flex flex-col min-h-0 p-3">
            {manualMount && session ? (
                <DownloadingFileTree disk={session} mountPath={manualMount} rootLabel="" />
            ) : mountError ? (
                <div className="flex-1 flex flex-col items-center justify-center gap-3 p-6 text-center">
                    <div className="text-base text-red-600 dark:text-red-400">
                        Can&rsquo;t mount partition #{selectedPart}
                    </div>
                    <div className="text-sm text-zinc-600 dark:text-zinc-400 font-mono max-w-lg break-words">
                        {mountError}
                    </div>
                    <div className="text-xs text-zinc-500 max-w-lg">
                        The filesystem on this partition isn&rsquo;t recognised by the bundled
                        kernel, or its on-disk format isn&rsquo;t supported. Pick a different
                        partition from the list above.
                    </div>
                    <button
                        className="mt-2 text-sm px-3 py-1.5 rounded bg-zinc-200 dark:bg-zinc-700 hover:bg-zinc-300 dark:hover:bg-zinc-600"
                        onClick={() => setSelectedPart(null)}
                    >
                        Back to partitions
                    </button>
                </div>
            ) : (
                <div className="flex-1 flex items-center justify-center text-base text-zinc-500">
                    mounting partition #{selectedPart}…
                </div>
            )}
        </section>
    );
}
