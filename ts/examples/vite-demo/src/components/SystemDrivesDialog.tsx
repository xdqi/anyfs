import { useCallback, useEffect, useState } from 'react';
import { formatSize } from '@anyfs/core';
import type { SysDrive, ElectronDrives } from './AboutDialog';

export function getElectronDrives(): ElectronDrives | null {
    const w = window as unknown as { electronDrives?: ElectronDrives };
    return w.electronDrives ?? null;
}

export type ElectronDialog = { openImage: () => Promise<string | null> };
export function getElectronDialog(): ElectronDialog | null {
    const w = window as unknown as { electronDialog?: ElectronDialog };
    return w.electronDialog ?? null;
}

// Modal SystemDrives picker. Triggered from the landing "Open system drive…"
// link. Lists physical disks + partitions; clicking a row emits a path
// SessionSource the native bridge will hand to the in-process LKL kernel.
//
// Whole-disk rows are clickable too — useful for partitionless block devices
// (loopback, optical, removable) and for letting the user lean on the
// kernel's own partition table parsing.
export function SystemDrivesDialog({
    onPick,
    onClose,
}: {
    onPick: (path: string, name: string) => void | Promise<void>;
    onClose: () => void;
}) {
    const bridge = getElectronDrives();
    const [drives, setDrives] = useState<SysDrive[] | null>(null);
    const [loading, setLoading] = useState(false);
    const [err, setErr] = useState<string | null>(null);

    const refresh = useCallback(async () => {
        if (!bridge) return;
        setLoading(true);
        setErr(null);
        try {
            const list = await bridge.list();
            setDrives(list ?? []);
        } catch (e) {
            setErr((e as Error).message);
        } finally {
            setLoading(false);
        }
    }, [bridge]);

    useEffect(() => {
        void refresh();
    }, [refresh]);

    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [onClose]);

    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onClose}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-2xl mx-4 shadow-xl max-h-[80vh] flex flex-col"
                onClick={(e) => e.stopPropagation()}
                role="dialog"
                aria-modal="true"
                aria-label="System drives"
            >
                <header className="px-5 py-4 border-b border-zinc-200 dark:border-zinc-800 flex items-center justify-between gap-3">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-lg font-semibold">
                        Open system drive
                    </h2>
                    <div className="flex items-center gap-3">
                        <button
                            type="button"
                            onClick={() => void refresh()}
                            disabled={loading}
                            className="text-xs uppercase tracking-wider text-zinc-500 hover:text-zinc-900 dark:hover:text-zinc-100 disabled:opacity-50"
                        >
                            {loading ? 'Scanning…' : 'Refresh'}
                        </button>
                        <button
                            className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 text-xl leading-none"
                            onClick={onClose}
                            aria-label="Close"
                        >
                            ×
                        </button>
                    </div>
                </header>
                <div className="overflow-y-auto p-4 space-y-3 flex-1">
                    {err && <div className="text-sm text-rose-500">{err}</div>}
                    {drives && drives.length === 0 && !loading && (
                        <div className="text-sm text-zinc-500">No drives detected.</div>
                    )}
                    {drives && drives.length > 0 && (
                        <ul className="rounded-xl border border-zinc-200 dark:border-zinc-800 overflow-hidden divide-y divide-zinc-200 dark:divide-zinc-800">
                            {drives.map((d) => (
                                <li key={d.device} className="p-3 space-y-1.5">
                                    <button
                                        type="button"
                                        onClick={() => {
                                            void onPick(
                                                d.device,
                                                `${d.device} (${formatSize(d.size ?? undefined)})`,
                                            );
                                        }}
                                        className="w-full flex items-baseline gap-2 text-sm hover:bg-zinc-100 dark:hover:bg-zinc-800 rounded p-1 -m-1 text-left"
                                    >
                                        <code className="text-zinc-900 dark:text-zinc-100 font-mono">
                                            {d.device}
                                        </code>
                                        <span className="text-zinc-500">
                                            {formatSize(d.size ?? undefined)}
                                        </span>
                                        <span className="text-zinc-500 truncate flex-1">
                                            {d.description}
                                        </span>
                                        {d.partitionTableType && (
                                            <span className="text-[10px] uppercase text-zinc-500">
                                                {d.partitionTableType}
                                            </span>
                                        )}
                                    </button>
                                    {d.partitions === null ? (
                                        <div className="text-xs text-zinc-500 pl-3">
                                            partitions unknown (platform binding not yet populated)
                                        </div>
                                    ) : d.partitions.length === 0 ? (
                                        <div className="text-xs text-zinc-500 pl-3">
                                            no partitions
                                        </div>
                                    ) : (
                                        <ul className="space-y-0.5 pl-3">
                                            {d.partitions.map((p) => {
                                                const known = p.fstype !== null;
                                                return (
                                                    <li key={p.device}>
                                                        <button
                                                            type="button"
                                                            onClick={() => {
                                                                void onPick(
                                                                    p.device,
                                                                    `${p.device}${
                                                                        p.label
                                                                            ? ` “${p.label}”`
                                                                            : ''
                                                                    }`,
                                                                );
                                                            }}
                                                            className="w-full flex items-baseline gap-2 text-xs hover:bg-zinc-100 dark:hover:bg-zinc-800 rounded p-1 -m-1 text-left"
                                                        >
                                                            <code
                                                                className={
                                                                    known
                                                                        ? 'text-zinc-700 dark:text-zinc-300 font-mono'
                                                                        : 'text-zinc-500 font-mono italic'
                                                                }
                                                            >
                                                                {p.device}
                                                            </code>
                                                            <span className="text-zinc-500">
                                                                {formatSize(p.size ?? undefined)}
                                                            </span>
                                                            <span
                                                                className={
                                                                    known
                                                                        ? 'text-emerald-600 dark:text-emerald-400'
                                                                        : 'text-amber-500'
                                                                }
                                                            >
                                                                {p.fstype ?? 'unknown'}
                                                            </span>
                                                            {p.label && (
                                                                <span className="text-zinc-700 dark:text-zinc-300 truncate">
                                                                    “{p.label}”
                                                                </span>
                                                            )}
                                                            {p.mountpoints.length > 0 && (
                                                                <span className="text-zinc-500 truncate">
                                                                    @{' '}
                                                                    {p.mountpoints
                                                                        .map((m) => m.path)
                                                                        .join(', ')}
                                                                </span>
                                                            )}
                                                        </button>
                                                    </li>
                                                );
                                            })}
                                        </ul>
                                    )}
                                </li>
                            ))}
                        </ul>
                    )}
                    <p className="text-xs text-zinc-500 leading-relaxed">
                        Opening a system disk needs read access to the raw device node (admin on
                        Windows, root or a group like <code>disk</code> on Linux). You may need to
                        run the demo with the right capabilities — or use a loopback image instead.
                    </p>
                </div>
            </div>
        </div>
    );
}
