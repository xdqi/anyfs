import { useCallback, useEffect, useState } from 'react';
import type { DragEvent as ReactDragEvent, ReactNode } from 'react';
import { AnyfsProvider, useAnyfsDisk, useAnyfsDiskMaybe } from '@anyfs/react';
import { AnyfsFileBrowser } from '@anyfs/trees';
import type { AnyfsDisk, DiskMeta, DiskSource, PartInfo } from '@anyfs/core';
import { streamDownload } from './stream-download';
import { SettingsDialog, SettingsProvider, useSettings } from './Settings';
import {
    addRecentFile,
    addRecentUrl,
    listRecents,
    removeRecent,
    tryReopen,
    fsaSupported,
    pickFile,
    type Recent,
} from './recents';

// The wasm lives inside a dedicated Web Worker (required: WORKERFS asserts
// ENVIRONMENT_IS_WORKER, and LKL sem_wait → Atomics.wait is blocked on
// Chrome's main thread). The worker script and wasm bundle are served from
// /public/wasm/.
const WORKER_URL = new URL('/wasm/anyfs.worker.js', window.location.href).href;

// Wipe the in-disk navigation hash. AnyfsFileBrowser writes paths like
// `#/etc/os-release`; when we tear down the disk or step back out of a
// partition, that hash no longer points anywhere meaningful and would
// otherwise stick around in the address bar (and re-seed the next mount
// with a stale path).
function clearNavHash() {
    if (typeof window === 'undefined') return;
    if (!window.location.hash) return;
    window.history.replaceState(null, '', window.location.pathname + window.location.search);
}

// User-facing label for a DiskSource. For files it's the on-disk filename;
// for URLs we lift the last path segment (decoded), falling back to host or
// the raw URL if the path is empty.
function sourceName(s: DiskSource): string {
    if (s.kind === 'file') return s.file.name;
    try {
        const u = new URL(s.url, window.location.href);
        const last = u.pathname.split('/').filter(Boolean).pop();
        if (last) return decodeURIComponent(last);
        return u.host || s.url;
    } catch {
        return s.url;
    }
}

interface ConfirmCfg {
    title: string;
    message: string;
    confirmLabel: string;
    onConfirm: () => void;
}

export function App() {
    const [source, setSource] = useState<DiskSource | null>(null);
    const [selectedPart, setSelectedPart] = useState<number | null>(null);
    const [settingsOpen, setSettingsOpen] = useState(false);
    const [aboutOpen, setAboutOpen] = useState(false);
    const [confirm, setConfirm] = useState<ConfirmCfg | null>(null);

    // Step back to the partition picker (multi-partition image only). Doesn't
    // unmount on the LKL side — re-entering the same partition just reuses
    // the cached mount path. We still gate behind a confirm because the user
    // explicitly asked for the prompt, and to make the gesture symmetric
    // with the close-disk prompt.
    const askBackToParts = () =>
        setConfirm({
            title: 'Return to the partition list?',
            message:
                'Leave this partition view and pick another one. Nothing is unmounted — you can come back.',
            confirmLabel: 'Back',
            onConfirm: () => {
                clearNavHash();
                setSelectedPart(null);
                setConfirm(null);
            },
        });

    const askCloseDisk = () =>
        setConfirm({
            title: 'Close this disk?',
            message:
                'Detach the current image and return to the picker. Any in-progress download will be cancelled.',
            confirmLabel: 'Close',
            onConfirm: () => {
                clearNavHash();
                setSelectedPart(null);
                setSource(null);
                setConfirm(null);
            },
        });

    // "anyfs reader" title always goes home (with confirm when a disk is
    // attached). The disk and partition crumbs are rendered alongside it
    // in the top bar; clicking the disk crumb steps back to the partition
    // picker (only when we're inside a partition).
    const onHomeClick = source ? askCloseDisk : undefined;
    const onImageClick = source && selectedPart !== null ? askBackToParts : undefined;

    return (
        <SettingsProvider>
            <AnyfsProvider
                source={source}
                workerUrl={WORKER_URL}
                wasmBaseUrl="/wasm/"
                wasmModuleName="anyfs.qemu.mjs"
                autoMountFstype="ext4"
                mountOpts={{ loglevel: 7 }}
                prewarm
            >
                <div className="h-screen flex flex-col">
                    <TopBar
                        onOpenSettings={() => setSettingsOpen(true)}
                        onOpenAbout={() => setAboutOpen(true)}
                        {...(onHomeClick ? { onHomeClick } : {})}
                        {...(source ? { imageName: sourceName(source) } : {})}
                        {...(onImageClick ? { onImageClick } : {})}
                        {...(selectedPart !== null
                            ? { partitionLabel: `partition #${selectedPart}` }
                            : {})}
                    />
                    <main className="flex-1 min-h-0 flex flex-col overflow-y-auto">
                        {source ? (
                            <DiskView
                                source={source}
                                selectedPart={selectedPart}
                                setSelectedPart={setSelectedPart}
                            />
                        ) : (
                            <FilePicker onSource={setSource} />
                        )}
                    </main>
                    <KernelStatusBar />
                    <SettingsDialog open={settingsOpen} onClose={() => setSettingsOpen(false)} />
                    <AboutDialog open={aboutOpen} onClose={() => setAboutOpen(false)} />
                    {confirm && (
                        <ConfirmDialog
                            title={confirm.title}
                            message={confirm.message}
                            confirmLabel={confirm.confirmLabel}
                            onCancel={() => setConfirm(null)}
                            onConfirm={confirm.onConfirm}
                        />
                    )}
                </div>
            </AnyfsProvider>
        </SettingsProvider>
    );
}

function TopBar({
    onOpenSettings,
    onOpenAbout,
    onHomeClick,
    imageName,
    onImageClick,
    partitionLabel,
}: {
    onOpenSettings: () => void;
    onOpenAbout: () => void;
    onHomeClick?: () => void;
    imageName?: string;
    onImageClick?: () => void;
    partitionLabel?: string;
}) {
    const titleClasses = 'text-zinc-900 dark:text-zinc-100 text-base font-semibold tracking-tight';
    const crumbBtn =
        'text-zinc-700 dark:text-zinc-300 text-base hover:text-emerald-600 dark:hover:text-emerald-400 transition-colors max-w-[20rem] truncate';
    const crumbCur = 'text-zinc-500 dark:text-zinc-400 text-base max-w-[20rem] truncate';
    const sep = (
        <span aria-hidden="true" className="text-zinc-400 dark:text-zinc-600 select-none">
            ›
        </span>
    );
    return (
        <header className="border-b border-zinc-200 dark:border-zinc-800 bg-white dark:bg-zinc-950 px-4 h-14 flex items-center justify-between gap-3">
            <nav aria-label="Breadcrumb" className="flex items-center gap-2 min-w-0">
                {onHomeClick ? (
                    <button
                        className={`${titleClasses} hover:text-emerald-600 dark:hover:text-emerald-400 transition-colors`}
                        onClick={onHomeClick}
                        title="Close this disk and return to the picker"
                    >
                        anyfs reader
                    </button>
                ) : (
                    <span className={titleClasses}>anyfs reader</span>
                )}
                {imageName && (
                    <>
                        {sep}
                        {onImageClick ? (
                            <button
                                className={crumbBtn}
                                onClick={onImageClick}
                                title="Return to the partition list"
                            >
                                {imageName}
                            </button>
                        ) : (
                            <span className={crumbCur} title={imageName}>
                                {imageName}
                            </span>
                        )}
                    </>
                )}
                {partitionLabel && (
                    <>
                        {sep}
                        <span className={crumbCur}>{partitionLabel}</span>
                    </>
                )}
            </nav>
            <div className="flex items-center gap-1 shrink-0">
                <a
                    href="https://github.com/xdqi/anyfs"
                    target="_blank"
                    rel="noopener noreferrer"
                    className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 p-1.5 inline-flex items-center justify-center"
                    aria-label="View source on GitHub"
                    title="View source on GitHub"
                >
                    <svg
                        viewBox="0 0 16 16"
                        width="20"
                        height="20"
                        fill="currentColor"
                        aria-hidden="true"
                    >
                        <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0 0 16 8c0-4.42-3.58-8-8-8z" />
                    </svg>
                </a>
                <button
                    className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 p-1.5 inline-flex items-center justify-center"
                    onClick={onOpenAbout}
                    aria-label="About"
                    title="About"
                >
                    <svg
                        viewBox="0 0 16 16"
                        width="20"
                        height="20"
                        fill="none"
                        stroke="currentColor"
                        strokeWidth="1.5"
                        aria-hidden="true"
                    >
                        <circle cx="8" cy="8" r="6.5" />
                        <line x1="8" y1="7" x2="8" y2="11.5" strokeLinecap="round" />
                        <circle cx="8" cy="4.7" r="0.85" fill="currentColor" stroke="none" />
                    </svg>
                </button>
                <button
                    className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 text-xl leading-none p-1"
                    onClick={onOpenSettings}
                    aria-label="Settings"
                    title="Settings"
                >
                    ⚙
                </button>
            </div>
        </header>
    );
}

function ConfirmDialog({
    title,
    message,
    confirmLabel = 'Confirm',
    onConfirm,
    onCancel,
}: {
    title: string;
    message: string;
    confirmLabel?: string;
    onConfirm: () => void;
    onCancel: () => void;
}) {
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onCancel();
            else if (e.key === 'Enter') onConfirm();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [onCancel, onConfirm]);
    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onCancel}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-md mx-4 shadow-xl"
                onClick={(e) => e.stopPropagation()}
                role="alertdialog"
                aria-modal="true"
            >
                <header className="px-5 py-4 border-b border-zinc-200 dark:border-zinc-800">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-lg font-semibold">
                        {title}
                    </h2>
                </header>
                <div className="px-5 py-4 text-base text-zinc-700 dark:text-zinc-300 leading-relaxed">
                    {message}
                </div>
                <div className="px-5 py-4 border-t border-zinc-200 dark:border-zinc-800 flex justify-end gap-2">
                    <button
                        className="rounded-lg border border-zinc-300 dark:border-zinc-700 px-4 py-2 text-sm font-medium text-zinc-700 dark:text-zinc-200 hover:bg-zinc-100 dark:hover:bg-zinc-800"
                        onClick={onCancel}
                    >
                        Cancel
                    </button>
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={onConfirm}
                        autoFocus
                    >
                        {confirmLabel}
                    </button>
                </div>
            </div>
        </div>
    );
}

function AboutDialog({ open, onClose }: { open: boolean; onClose: () => void }) {
    useEffect(() => {
        if (!open) return;
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [open, onClose]);
    if (!open) return null;

    type Dep = { name: string; license: string; url: string; note?: string };
    const native: Dep[] = [
        {
            name: 'Linux Kernel Library (LKL)',
            license: 'GPL-2.0',
            url: 'https://github.com/lkl/linux',
            note: 'in-process Linux kernel — does all filesystem work',
        },
        {
            name: 'QEMU',
            license: 'GPL-2.0+',
            url: 'https://www.qemu.org',
            note: 'block layer — image format support',
        },
        {
            name: 'util-linux (libblkid)',
            license: 'LGPL-2.1+',
            url: 'https://github.com/util-linux/util-linux',
        },
        { name: 'GLib', license: 'LGPL-2.1+', url: 'https://gitlab.gnome.org/GNOME/glib' },
    ];
    const web: Dep[] = [
        { name: 'React', license: 'MIT', url: 'https://react.dev' },
        { name: 'Vite', license: 'MIT', url: 'https://vitejs.dev' },
        { name: 'Tailwind CSS', license: 'MIT', url: 'https://tailwindcss.com' },
        { name: 'Chonky', license: 'MIT', url: 'https://github.com/TimboKZ/Chonky' },
        {
            name: 'chonky-icon-fontawesome',
            license: 'MIT',
            url: 'https://github.com/TimboKZ/Chonky',
        },
    ];

    const linkCls = 'text-emerald-700 dark:text-emerald-400 hover:underline';
    const sectionH =
        'text-xs font-semibold uppercase tracking-wide text-zinc-500 dark:text-zinc-400 mt-4 mb-2';

    const renderList = (deps: Dep[]) => (
        <ul className="space-y-1.5">
            {deps.map((d) => (
                <li
                    key={d.name}
                    className="text-sm text-zinc-700 dark:text-zinc-300 flex items-baseline gap-2 flex-wrap"
                >
                    <a href={d.url} target="_blank" rel="noopener noreferrer" className={linkCls}>
                        {d.name}
                    </a>
                    <span className="text-xs text-zinc-500 dark:text-zinc-400 font-mono">
                        {d.license}
                    </span>
                    {d.note && (
                        <span className="text-xs text-zinc-500 dark:text-zinc-400">— {d.note}</span>
                    )}
                </li>
            ))}
        </ul>
    );

    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onClose}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-lg mx-4 shadow-xl flex flex-col max-h-[85vh]"
                onClick={(e) => e.stopPropagation()}
                role="dialog"
                aria-modal="true"
                aria-labelledby="about-dialog-title"
            >
                <header className="px-5 py-4 border-b border-zinc-200 dark:border-zinc-800">
                    <h2
                        id="about-dialog-title"
                        className="text-zinc-900 dark:text-zinc-100 text-lg font-semibold"
                    >
                        About anyfs reader
                    </h2>
                </header>
                <div className="px-5 py-4 overflow-y-auto">
                    <p className="text-sm text-zinc-700 dark:text-zinc-300 leading-relaxed">
                        Read any Linux-supported filesystem from your browser — no root, no FUSE, no
                        kernel modules. Powered by a Linux kernel running in-tab via WebAssembly.
                    </p>
                    <div className={sectionH}>License</div>
                    <p className="text-sm text-zinc-700 dark:text-zinc-300 leading-relaxed">
                        Licensed under the{' '}
                        <a
                            href="https://www.gnu.org/licenses/old-licenses/gpl-2.0.html"
                            target="_blank"
                            rel="noopener noreferrer"
                            className={linkCls}
                        >
                            GNU General Public License v2.0
                        </a>{' '}
                        (GPL-2.0). Source code is available on GitHub.
                    </p>
                    <div className={sectionH}>Native components</div>
                    {renderList(native)}
                    <div className={sectionH}>Web stack</div>
                    {renderList(web)}
                    <p className="mt-4 text-xs text-zinc-500 dark:text-zinc-400 leading-relaxed">
                        Each component is the property of its respective authors and is used under
                        its own license. Visit the linked project pages for the full license text
                        and copyright notices.
                    </p>
                </div>
                <div className="px-5 py-4 border-t border-zinc-200 dark:border-zinc-800 flex justify-end">
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={onClose}
                        autoFocus
                    >
                        Close
                    </button>
                </div>
            </div>
        </div>
    );
}

// Probe a URL via async HEAD on the main thread. Returns the size on
// success; throws a user-readable Error on failure. We do this here
// (before handing off to the worker) so we can show a real dialog
// instead of an attach-time crash buried in the status bar.
async function probeUrlAhead(url: string): Promise<number> {
    let resp: Response;
    try {
        resp = await fetch(url, { method: 'HEAD', cache: 'no-store' });
    } catch (e) {
        // Most commonly: CORS rejection or DNS/connection failure. fetch()
        // collapses both into an opaque TypeError — we can't tell them apart
        // from JS, but "can't reach + no CORS" is the message either way.
        throw new Error(
            `Couldn't reach the URL — usually CORS (the server didn't send ` +
                `Access-Control-Allow-Origin), or the host is down. ` +
                `Browser console has the real error.`,
            { cause: e },
        );
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
        // Mirror URLFS's fallback: some servers omit the header but still
        // honour Range. Probe with a 1-byte ranged GET and check for 206.
        let probe: Response;
        try {
            probe = await fetch(url, {
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

function FilePicker({ onSource }: { onSource: (s: DiskSource) => void }) {
    const [dragging, setDragging] = useState(false);
    const [url, setUrl] = useState('');
    const [probing, setProbing] = useState(false);
    const [errorDialog, setErrorDialog] = useState<{ title: string; message: string } | null>(null);
    const [recents, setRecents] = useState<Recent[]>([]);
    const fsa = fsaSupported();

    const refreshRecents = useCallback(async () => {
        setRecents(await listRecents());
    }, []);
    useEffect(() => {
        void refreshRecents();
    }, [refreshRecents]);

    // Always-on path: produce a DiskSource and stash a recents entry when we
    // have a persistable handle. The handle is optional — drops without
    // getAsFileSystemHandle (or the legacy <input> fallback on non-FSA
    // browsers) still mount, they just won't show up in Recents.
    const acceptFile = useCallback(
        async (file: File, handle?: FileSystemFileHandle) => {
            if (handle) {
                try {
                    await addRecentFile(handle, file.name, file.size);
                } catch {}
                void refreshRecents();
            }
            onSource({ kind: 'file', file });
        },
        [onSource, refreshRecents],
    );

    const submitUrl = async () => {
        const trimmed = url.trim();
        if (!trimmed) return;
        setProbing(true);
        try {
            const size = await probeUrlAhead(trimmed);
            const name = sourceName({ kind: 'url', url: trimmed });
            try {
                await addRecentUrl(trimmed, name, size);
            } catch {}
            void refreshRecents();
            onSource({ kind: 'url', url: trimmed });
        } catch (e) {
            setErrorDialog({
                title: 'Can’t load this URL',
                message: (e as Error).message,
            });
        } finally {
            setProbing(false);
        }
    };

    // Try to lift a FileSystemFileHandle out of a drop event so dropped files
    // also become recents on Chrome/Edge. Falls back to dataTransfer.files
    // (still mounts, just doesn't persist).
    const onDrop = async (e: ReactDragEvent) => {
        e.preventDefault();
        setDragging(false);
        if (fsa && e.dataTransfer.items && e.dataTransfer.items.length > 0) {
            for (const item of Array.from(e.dataTransfer.items)) {
                if (item.kind !== 'file') continue;
                // Non-standard but shipped in Chromium/WebKit; cast to surface it.
                const getHandle = (
                    item as DataTransferItem & {
                        getAsFileSystemHandle?: () => Promise<FileSystemHandle | null>;
                    }
                ).getAsFileSystemHandle;
                if (getHandle) {
                    try {
                        const h = await getHandle.call(item);
                        if (h && h.kind === 'file') {
                            const fh = h as FileSystemFileHandle;
                            const f = await fh.getFile();
                            await acceptFile(f, fh);
                            return;
                        }
                    } catch {}
                }
            }
        }
        const f = e.dataTransfer.files[0];
        if (f) await acceptFile(f);
    };

    // Native picker. With FSA we get a handle; without it we trigger a hidden
    // <input id="anyfs-legacy-file"> rendered inside the drop zone.
    const onBrowseClick = async () => {
        try {
            if (fsa) {
                const picked = await pickFile();
                if (!picked) return;
                await acceptFile(picked.file, picked.handle);
                return;
            }
            // Non-FSA fallback: trigger the hidden <input>.
            const el = document.getElementById('anyfs-legacy-file') as HTMLInputElement | null;
            el?.click();
        } catch (e) {
            setErrorDialog({ title: 'Can’t open file', message: (e as Error).message });
        }
    };

    const onReopen = async (r: Recent) => {
        try {
            const res = await tryReopen(r);
            if (res.kind === 'ok') {
                if (res.source.kind === 'url') {
                    // Re-probe so we can surface CORS / 404 failures here
                    // rather than as a worker crash. Skip for files.
                    try {
                        await probeUrlAhead(res.source.url);
                    } catch (e) {
                        setErrorDialog({
                            title: 'Can’t reopen URL',
                            message: (e as Error).message,
                        });
                        return;
                    }
                }
                await refreshRecents();
                onSource(res.source);
                return;
            }
            if (res.kind === 'missing') {
                setErrorDialog({
                    title: 'File no longer available',
                    message: `“${r.name}” was moved or deleted since you opened it. Removing from Recents.`,
                });
                await removeRecent(r.id);
                await refreshRecents();
                return;
            }
            // denied
            setErrorDialog({
                title: 'Permission needed',
                message:
                    'The browser blocked re-opening this file. Click the Recents item again and choose “Allow” in the prompt.',
            });
        } catch (e) {
            setErrorDialog({ title: 'Can’t reopen', message: (e as Error).message });
        }
    };

    const onRemove = async (id: string) => {
        await removeRecent(id);
        await refreshRecents();
    };

    const urlValid = url.trim().length > 0 && !probing;
    return (
        <div className="flex-1 flex items-center justify-center p-6">
            <div className="w-full max-w-xl rounded-2xl bg-white border border-zinc-200 dark:bg-zinc-900 dark:border-zinc-800 p-6 space-y-4 shadow-xl">
                {recents.length > 0 && (
                    <RecentsList recents={recents} onReopen={onReopen} onRemove={onRemove} />
                )}
                <div
                    onDragEnter={(e) => {
                        e.preventDefault();
                        setDragging(true);
                    }}
                    onDragOver={(e) => {
                        e.preventDefault();
                        setDragging(true);
                    }}
                    onDragLeave={() => setDragging(false)}
                    onDrop={(e) => {
                        void onDrop(e);
                    }}
                    onClick={() => {
                        void onBrowseClick();
                    }}
                    role="button"
                    tabIndex={0}
                    onKeyDown={(e) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                            e.preventDefault();
                            void onBrowseClick();
                        }
                    }}
                    className={[
                        'block rounded-xl border-2 border-dashed cursor-pointer',
                        'py-12 px-6 text-center transition-colors',
                        dragging
                            ? 'border-emerald-500 bg-emerald-500/10'
                            : 'border-zinc-300 hover:border-zinc-400 dark:border-zinc-700 dark:hover:border-zinc-500',
                    ].join(' ')}
                >
                    {!fsa && (
                        <input
                            id="anyfs-legacy-file"
                            type="file"
                            className="hidden"
                            onChange={(e) => {
                                const f = e.target.files?.[0];
                                if (f) void acceptFile(f);
                            }}
                        />
                    )}
                    <p className="text-xl text-zinc-800 dark:text-zinc-200">
                        Drop a disk image here
                    </p>
                    <p className="text-base text-zinc-500 mt-1">or click to browse</p>
                </div>
                <div className="flex items-center gap-3 text-xs text-zinc-500 uppercase tracking-wider">
                    <div className="flex-1 h-px bg-zinc-200 dark:bg-zinc-800"></div>
                    <span>OR</span>
                    <div className="flex-1 h-px bg-zinc-200 dark:bg-zinc-800"></div>
                </div>
                <div className="flex gap-2">
                    <input
                        type="url"
                        value={url}
                        onChange={(e) => setUrl(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === 'Enter') void submitUrl();
                        }}
                        placeholder="Paste image URL (https://…)"
                        disabled={probing}
                        className="flex-1 min-w-0 rounded-lg bg-zinc-100 border border-zinc-300 text-zinc-900 placeholder:text-zinc-400 dark:bg-zinc-800 dark:border-zinc-700 dark:text-zinc-100 dark:placeholder:text-zinc-500 px-3 py-2 text-base focus:outline-none focus:border-emerald-500 disabled:opacity-60"
                        aria-label="Disk image URL"
                    />
                    <button
                        type="button"
                        onClick={() => void submitUrl()}
                        disabled={!urlValid}
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 disabled:bg-zinc-300 disabled:text-zinc-500 dark:disabled:bg-zinc-700 dark:disabled:text-zinc-500 px-4 py-2 text-base font-medium text-white transition-colors min-w-20"
                    >
                        {probing ? 'Probing…' : 'Load'}
                    </button>
                </div>
                <p className="text-sm text-zinc-500 leading-relaxed">
                    URLs are fetched in 512&nbsp;KiB chunks via HTTP <code>Range</code> requests —
                    the server must support range responses and CORS.
                </p>
                <SupportedFormats />
            </div>
            {errorDialog && (
                <UrlErrorDialog
                    title={errorDialog.title}
                    message={errorDialog.message}
                    onClose={() => setErrorDialog(null)}
                />
            )}
        </div>
    );
}

function UrlErrorDialog({
    title,
    message,
    onClose,
}: {
    title: string;
    message: string;
    onClose: () => void;
}) {
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
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-md mx-4 shadow-xl"
                onClick={(e) => e.stopPropagation()}
                role="alertdialog"
                aria-modal="true"
            >
                <header className="px-4 py-3 border-b border-zinc-200 dark:border-zinc-800 flex items-center justify-between">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-base font-medium">
                        {title}
                    </h2>
                    <button
                        className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100"
                        onClick={onClose}
                        aria-label="Close"
                    >
                        ×
                    </button>
                </header>
                <div className="p-4 text-sm text-zinc-700 dark:text-zinc-300 leading-relaxed">
                    {message}
                </div>
                <div className="px-4 py-3 border-t border-zinc-200 dark:border-zinc-800 flex justify-end">
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={onClose}
                        autoFocus
                    >
                        OK
                    </button>
                </div>
            </div>
        </div>
    );
}

function formatSize(n: number | undefined): string {
    if (n === undefined || !Number.isFinite(n)) return '';
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let v = n;
    let u = 0;
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024;
        u++;
    }
    return `${v < 10 && u > 0 ? v.toFixed(1) : Math.round(v)} ${units[u]}`;
}

function formatTs(ts: number): string {
    const diff = Date.now() - ts;
    const m = Math.floor(diff / 60000);
    if (m < 1) return 'just now';
    if (m < 60) return `${m}m ago`;
    const h = Math.floor(m / 60);
    if (h < 24) return `${h}h ago`;
    const d = Math.floor(h / 24);
    if (d < 30) return `${d}d ago`;
    return new Date(ts).toLocaleDateString();
}

function RecentsList({
    recents,
    onReopen,
    onRemove,
}: {
    recents: Recent[];
    onReopen: (r: Recent) => void | Promise<void>;
    onRemove: (id: string) => void | Promise<void>;
}) {
    return (
        <div className="space-y-1.5">
            <div className="text-[10px] uppercase tracking-wider text-zinc-500 dark:text-zinc-400 px-1">
                Recently opened
            </div>
            <ul className="rounded-xl border border-zinc-200 dark:border-zinc-800 overflow-hidden divide-y divide-zinc-200 dark:divide-zinc-800 bg-zinc-50 dark:bg-zinc-900/50">
                {recents.map((r) => (
                    <li
                        key={r.id}
                        className="flex items-center gap-2 px-3 py-2 hover:bg-zinc-100 dark:hover:bg-zinc-800/70 transition-colors group"
                    >
                        <button
                            type="button"
                            onClick={() => {
                                void onReopen(r);
                            }}
                            title={
                                r.kind === 'url'
                                    ? r.url
                                    : `${r.name} (local file — browsers don’t expose paths)`
                            }
                            className="flex-1 min-w-0 flex items-center gap-2 text-left"
                        >
                            <span className="shrink-0 text-base">
                                {r.kind === 'url' ? '🌐' : '💾'}
                            </span>
                            <span className="flex-1 min-w-0">
                                <span className="block truncate text-sm text-zinc-800 dark:text-zinc-200">
                                    {r.name}
                                </span>
                                {r.kind === 'url' ? (
                                    <span
                                        className="block truncate text-[11px] font-mono text-zinc-500 dark:text-zinc-400"
                                        dir="ltr"
                                    >
                                        {r.url}
                                    </span>
                                ) : (
                                    <span className="block truncate text-[11px] italic text-zinc-400 dark:text-zinc-500">
                                        local file · path hidden by browser
                                    </span>
                                )}
                                <span className="block text-[11px] text-zinc-500 dark:text-zinc-400">
                                    {formatTs(r.ts)}
                                    {r.size !== undefined ? ` · ${formatSize(r.size)}` : ''}
                                </span>
                            </span>
                        </button>
                        <button
                            type="button"
                            onClick={(e) => {
                                e.stopPropagation();
                                void onRemove(r.id);
                            }}
                            className="shrink-0 w-6 h-6 rounded text-zinc-400 hover:text-zinc-700 hover:bg-zinc-200 dark:hover:text-zinc-100 dark:hover:bg-zinc-700 opacity-0 group-hover:opacity-100 focus:opacity-100 transition-opacity"
                            aria-label={`Remove ${r.name} from recents`}
                            title="Remove from recents"
                        >
                            ×
                        </button>
                    </li>
                ))}
            </ul>
        </div>
    );
}

// Surfaced on the landing card so users know up front what we can open.
// Image formats come from the wasm QEMU bundle (build-anyfs-wasm/libblock.a);
// filesystems come from the LKL kernel config — the list below mirrors the
// CONFIG_*_FS=y set actually built into lkl-wasm/.config (run
// `grep _FS=y lkl-wasm/.config` to verify). XFS is the one mainline FS off
// (wasm32 computed-goto limitation); REISERFS / BCACHEFS / SYSV silently
// drop out of the kernel build and aren't listed.
// Image formats come from QEMU's block layer, not the Linux FS registry,
// so they have to stay hardcoded — there's no kernel pseudo-file for them.
// Mirrors the format drivers actually inside libblock.a; to verify run:
//   ls $HOME/qemu/build-anyfs-wasm/libblock.a.p/ | grep '^block_[a-z]\+\.c\.o$'
const IMAGE_FORMATS: Array<{ name: string; title?: string }> = [
    { name: 'raw' },
    { name: 'qcow2' },
    { name: 'qcow', title: 'qcow v1 (legacy, obsoleted by qcow2)' },
    { name: 'vmdk', title: 'VMware Virtual Machine Disk' },
    { name: 'vdi', title: 'VirtualBox Disk Image' },
    { name: 'vhd', title: 'Microsoft Virtual Hard Disk (fixed/dynamic, QEMU vpc driver)' },
    { name: 'vhdx', title: 'Microsoft Hyper-V Virtual Hard Disk v2' },
    {
        name: 'dmg',
        title: 'Apple Disk Image (UDIF, incl. UDBZ bzip2; lzfse-compressed chunks unsupported)',
    },
    { name: 'qed', title: 'QEMU Enhanced Disk (deprecated)' },
    { name: 'parallels', title: 'Parallels Desktop disk image' },
    { name: 'bochs', title: 'Bochs emulator disk image' },
    { name: 'cloop', title: 'Compressed Loopback (Knoppix)' },
];

// Fallback FS list shown when the kernel hasn't booted yet (or /proc/filesystems
// read fails). Should track CONFIG_*_FS=y in scripts/gen_lkl_config.sh.
const FALLBACK_FS = [
    'ext2',
    'ext3',
    'ext4',
    'btrfs',
    'f2fs',
    'jfs',
    'gfs2',
    'nilfs2',
    'vfat',
    'exfat',
    'ntfs3',
    'hfs',
    'hfsplus',
    'iso9660',
    'udf',
    'squashfs',
    'erofs',
    'cramfs',
    'minix',
    'romfs',
    'ufs',
    'hpfs',
    'qnx4',
    'qnx6',
    'bfs',
    'omfs',
    'befs',
    'affs',
    'adfs',
    'efs',
    'vxfs',
];

// Pretty group label + name normalization based on the canonical kernel name.
const FS_GROUPS: Array<{ label: string; members: Record<string, string> }> = [
    {
        label: 'Mainstream',
        members: {
            ext2: 'ext2',
            ext3: 'ext3',
            ext4: 'ext4',
            btrfs: 'BTRFS',
            xfs: 'XFS',
            f2fs: 'F2FS',
            jfs: 'JFS',
            gfs2: 'GFS2',
            nilfs2: 'NILFS2',
        },
    },
    {
        label: 'Windows / macOS',
        members: {
            vfat: 'VFAT',
            msdos: 'MS-DOS',
            exfat: 'exFAT',
            ntfs3: 'NTFS',
            ntfs: 'NTFS',
            hfs: 'HFS',
            hfsplus: 'HFS+',
            apfs: 'APFS',
        },
    },
    {
        label: 'Optical / archive',
        members: {
            iso9660: 'ISO 9660',
            udf: 'UDF',
            squashfs: 'SquashFS',
            erofs: 'EROFS',
            cramfs: 'CRAMFS',
        },
    },
    {
        label: 'Legacy Unix',
        members: {
            minix: 'MINIX',
            romfs: 'ROMFS',
            ufs: 'UFS',
            hpfs: 'HPFS',
            qnx4: 'QNX4',
            qnx6: 'QNX6',
            bfs: 'BFS',
            omfs: 'OMFS',
            befs: 'BeFS',
            affs: 'AFFS',
            adfs: 'ADFS',
            efs: 'EFS',
            vxfs: 'VxFS',
        },
    },
];

// /proc/filesystems format: each line is either `nodev\t<name>` (pseudo-FS
// like proc/sysfs/tmpfs) or `\t<name>` (block-backed real FS). We only want
// the block-backed ones — the nodev set is full of internal kernel mounts
// that have nothing to do with reading a disk image.
function parseProcFilesystems(text: string): string[] {
    const out: string[] = [];
    for (const line of text.split('\n')) {
        if (!line) continue;
        const parts = line.split('\t');
        if (parts.length < 2) continue;
        const flag = parts[0]!.trim();
        const name = parts[1]!.trim();
        if (flag === 'nodev') continue;
        if (!name) continue;
        out.push(name);
    }
    return out;
}

function SupportedFormats() {
    const { kernel, status } = useAnyfsDisk();
    const [kernelFs, setKernelFs] = useState<string[] | null>(null);

    useEffect(() => {
        if (!kernel) return;
        let cancelled = false;
        (async () => {
            try {
                const txt = await kernel.readKernelFile('/proc/filesystems');
                if (cancelled) return;
                setKernelFs(parseProcFilesystems(txt));
            } catch {
                // Kernel not ready or read failed — leave fallback in place.
            }
        })();
        return () => {
            cancelled = true;
        };
    }, [kernel]);

    const fsList = kernelFs ?? FALLBACK_FS;
    const fsSet = new Set(fsList);
    // Track which kernel names we've already shown so the "Other" bucket
    // catches anything compiled in but not categorized above.
    const claimed = new Set<string>();

    const Chip = ({ children, title }: { children: ReactNode; title?: string }) => (
        <span
            className="inline-block rounded-md bg-zinc-100 dark:bg-zinc-800 border border-zinc-200 dark:border-zinc-700 px-1.5 py-0.5 text-[11px] font-mono text-zinc-700 dark:text-zinc-300"
            title={title}
        >
            {children}
        </span>
    );
    const Group = ({
        label,
        chips,
    }: {
        label: string;
        chips: Array<{ key: string; text: string; title?: string }>;
    }) =>
        chips.length === 0 ? null : (
            <div>
                <div className="text-[10px] uppercase tracking-wider text-zinc-500 dark:text-zinc-400 mb-1">
                    {label}
                </div>
                <div className="flex flex-wrap gap-1">
                    {chips.map((c) => (
                        <Chip key={c.key} title={c.title}>
                            {c.text}
                        </Chip>
                    ))}
                </div>
            </div>
        );

    const groups = FS_GROUPS.map((g) => {
        const chips: Array<{ key: string; text: string; title?: string }> = [];
        // Coalesce ext2/3/4 into one chip when all three are present.
        if (
            g.label === 'Mainstream' &&
            fsSet.has('ext2') &&
            fsSet.has('ext3') &&
            fsSet.has('ext4')
        ) {
            chips.push({ key: 'ext234', text: 'ext2/3/4', title: 'ext2, ext3, ext4' });
            claimed.add('ext2');
            claimed.add('ext3');
            claimed.add('ext4');
        }
        for (const [name, label] of Object.entries(g.members)) {
            if (claimed.has(name)) continue;
            if (!fsSet.has(name)) continue;
            chips.push({ key: name, text: label, title: name });
            claimed.add(name);
        }
        return { label: g.label, chips };
    });
    const other = fsList.filter((n) => !claimed.has(n)).map((n) => ({ key: n, text: n }));

    const live = kernelFs !== null;
    const sourceNote = live
        ? `From /proc/filesystems (${fsList.length} filesystems)`
        : status === 'booting' || status === 'attaching' || status === 'mounting'
          ? 'Booting kernel — showing default list'
          : 'Default list — kernel not yet booted';

    return (
        <div className="pt-2 border-t border-zinc-200 dark:border-zinc-800 space-y-2">
            <Group
                label="Image formats"
                chips={IMAGE_FORMATS.map((f) => ({ key: f.name, text: f.name, title: f.title }))}
            />
            {groups.map((g) => (
                <Group key={g.label} label={g.label} chips={g.chips} />
            ))}
            <Group label="Other" chips={other} />
            <div className="text-[10px] text-zinc-500 dark:text-zinc-400 italic pt-0.5">
                {sourceNote}
            </div>
        </div>
    );
}

// `shrink-0 bg-* relative z-10` keeps the bar planted at the bottom and
// painted on top even when the page content above is taller than the
// viewport (e.g. the picker card with Recents + SupportedFormats expanded).
// Without the opaque bg, overflowing content peeks through the transparent
// bar; without shrink-0, flex layout can compress it on very short windows.
const STATUS_BAR_CLS =
    'shrink-0 relative z-10 bg-white dark:bg-zinc-900 border-t border-zinc-200 dark:border-zinc-800 px-4 py-1.5 text-sm';

function KernelStatusBar() {
    const anyfs = useAnyfsDiskMaybe();
    if (!anyfs)
        return <div className={`${STATUS_BAR_CLS} text-zinc-600 dark:text-zinc-400`}>idle</div>;
    let label: string;
    // Light-mode shades are darker (-600/700) so the text reads cleanly on a
    // white background; the -400 dark-mode shades keep the same vibe on zinc-900.
    let cls = 'text-zinc-600 dark:text-zinc-400';
    switch (anyfs.status) {
        case 'booting':
            label = `🔧 booting kernel${anyfs.step ? ` · ${anyfs.step}` : '…'}`;
            cls = 'text-amber-700 dark:text-amber-400';
            break;
        case 'booted':
            label = '✅ kernel ready';
            cls = 'text-emerald-700 dark:text-emerald-400';
            break;
        case 'attaching':
            label = `⏳ attaching disk${anyfs.step ? ` · ${anyfs.step}` : '…'}`;
            cls = 'text-amber-700 dark:text-amber-400';
            break;
        case 'mounting':
            label = `📂 ${anyfs.step ?? 'mounting filesystem…'}`;
            cls = 'text-amber-700 dark:text-amber-400';
            break;
        case 'ready':
            label = '✅ ready';
            cls = 'text-emerald-700 dark:text-emerald-400';
            break;
        case 'error':
            label = `⚠️ ${anyfs.error?.message ?? 'error'}`;
            cls = 'text-red-700 dark:text-red-400';
            break;
        default:
            label = anyfs.status;
    }
    return <div className={`${STATUS_BAR_CLS} ${cls}`}>{label}</div>;
}

function DiskView({
    source,
    selectedPart,
    setSelectedPart,
}: {
    source: DiskSource;
    selectedPart: number | null;
    setSelectedPart: (n: number | null) => void;
}) {
    const { disk, mountPath, status, step, error } = useAnyfsDisk();
    const [parts, setParts] = useState<PartInfo[] | null>(null);
    const [meta, setMeta] = useState<DiskMeta | null>(null);
    const [onDiskSize, setOnDiskSize] = useState<number | null>(null);
    const [manualMount, setManualMount] = useState<string | null>(null);

    // When App steps us back out of a partition (selectedPart → null) we
    // need to drop the cached mount path too, otherwise re-picking the same
    // partition shows the previous mount instead of remounting fresh.
    useEffect(() => {
        if (selectedPart === null) setManualMount(null);
    }, [selectedPart]);

    useEffect(() => {
        if (!disk || status !== 'ready') return;
        disk.listPartitions()
            .then(setParts)
            .catch(() => setParts([]));
        disk.diskMeta()
            .then(setMeta)
            .catch(() => setMeta(null));
    }, [disk, status]);

    // Resolve "on-disk" (raw container) size: File.size for local, a HEAD's
    // Content-Length for URL. Differs from logical_size for qcow2/etc.
    useEffect(() => {
        if (source.kind === 'file') {
            setOnDiskSize(source.file.size);
            return;
        }
        let cancelled = false;
        fetch(source.url, { method: 'HEAD', cache: 'no-store' })
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
        if (!disk || selectedPart === null) return;
        let cancelled = false;
        disk.enter(selectedPart).then((mp) => {
            if (!cancelled) setManualMount(mp);
        });
        return () => {
            cancelled = true;
        };
    }, [disk, selectedPart]);

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

    // Whole-disk: AnyfsProvider already mounted under mountPath.
    if (mountPath) {
        return (
            <section className="flex-1 flex flex-col min-h-0 p-3">
                <DownloadingFileTree disk={disk!} mountPath={mountPath} rootLabel="" />
            </section>
        );
    }

    if (parts === null)
        return (
            <div className="flex-1 flex items-center justify-center text-base text-zinc-600 dark:text-zinc-400">
                Reading partition table…
            </div>
        );
    if (parts.length === 0)
        return (
            <div className="flex-1 flex items-center justify-center text-base text-zinc-600 dark:text-zinc-400">
                No partition table and no auto-mount fstype matched.
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
            {manualMount && disk ? (
                <DownloadingFileTree disk={disk} mountPath={manualMount} rootLabel="" />
            ) : (
                <div className="flex-1 flex items-center justify-center text-base text-zinc-500">
                    mounting partition #{selectedPart}…
                </div>
            )}
        </section>
    );
}

function DownloadingFileTree({
    disk,
    mountPath,
    rootLabel,
}: {
    disk: AnyfsDisk;
    mountPath: string;
    rootLabel: string;
}) {
    const [active, setActive] = useState<DownloadJob | null>(null);
    const { settings, resolvedTheme } = useSettings();

    const startDownload = useCallback(
        async (relPath: string) => {
            const fileName = relPath.split('/').pop() || 'download.bin';
            // Resolve to LKL absolute path: mountPath + '/' + relPath
            const abs = mountPath.endsWith('/')
                ? `${mountPath}${relPath}`
                : `${mountPath}/${relPath}`;
            const { stream, size } = await disk.openReadable(abs);
            const job: DownloadJob = {
                name: fileName,
                size,
                written: 0,
                cancel: () => {},
            };

            const handle = streamDownload({
                stream,
                fileName,
                size,
                onProgress: (written) => {
                    job.written = written;
                    setActive({ ...job });
                },
            });
            job.cancel = () => handle.cancel();
            setActive(job);
            try {
                await handle.promise;
                setActive(null);
            } catch (err) {
                setActive({ ...job, error: (err as Error).message });
            }
        },
        [disk, mountPath],
    );

    return (
        <>
            <AnyfsFileBrowser
                disk={disk}
                mountPath={mountPath}
                rootLabel={rootLabel}
                followSymlinks={settings.followSymlinks}
                darkMode={resolvedTheme === 'dark'}
                className="rounded-md bg-zinc-100 dark:bg-zinc-800 p-2 flex-1 min-h-0"
                onFileActivate={({ relPath }) => {
                    void startDownload(relPath);
                }}
            />
            {active && <DownloadStatus job={active} onDismiss={() => setActive(null)} />}
        </>
    );
}

interface DownloadJob {
    name: string;
    size: number;
    written: number;
    cancel: () => void;
    error?: string;
}

function DownloadStatus({ job, onDismiss }: { job: DownloadJob; onDismiss: () => void }) {
    const pct = job.size > 0 ? Math.min(100, (job.written / job.size) * 100) : 0;
    return (
        <div className="rounded-md bg-zinc-100 dark:bg-zinc-800 px-4 py-3 text-base flex items-center gap-3 text-zinc-900 dark:text-zinc-100">
            <div className="flex-1 min-w-0">
                <div className="truncate">{job.name}</div>
                {job.error ? (
                    <div className="text-red-500 dark:text-red-400 text-sm mt-1">{job.error}</div>
                ) : (
                    <div className="text-zinc-600 dark:text-zinc-400 text-sm mt-1">
                        {fmtBytes(job.written)} / {fmtBytes(job.size)} ({pct.toFixed(1)}%)
                    </div>
                )}
                {!job.error && (
                    <div className="h-1.5 bg-zinc-300 dark:bg-zinc-700 mt-1.5 rounded overflow-hidden">
                        <div className="h-full bg-emerald-500" style={{ width: `${pct}%` }} />
                    </div>
                )}
            </div>
            {job.error ? (
                <button
                    className="text-zinc-700 hover:text-zinc-900 dark:text-zinc-300 dark:hover:text-white text-sm"
                    onClick={onDismiss}
                >
                    ×
                </button>
            ) : (
                <button
                    className="text-zinc-700 hover:text-zinc-900 dark:text-zinc-300 dark:hover:text-white text-sm"
                    onClick={() => {
                        job.cancel();
                        onDismiss();
                    }}
                >
                    cancel
                </button>
            )}
        </div>
    );
}

function fmtBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
    if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`;
    return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`;
}

function ptLabel(pt: string): string {
    const k = pt.toLowerCase();
    if (k === 'gpt') return 'GPT';
    if (k === 'dos' || k === 'mbr') return 'MBR';
    if (!k) return 'no partition table';
    return pt;
}

// Header rendered above the partition list: filename / URL, logical (virtual
// block device) size, container size on disk (Content-Length for URL, File.size
// for File — differs from logical for qcow2/vmdk/etc), and partition-table
// flavour. Best-effort; missing pieces hide.
function DiskSummary({
    source,
    meta,
    onDiskSize,
}: {
    source: DiskSource;
    meta: DiskMeta | null;
    onDiskSize: number | null;
}) {
    const name = sourceName(source);
    const logical = meta && meta.logical_size > 0 ? meta.logical_size : null;
    const compressed = onDiskSize !== null && logical !== null && onDiskSize < logical * 0.95;
    return (
        <div className="text-sm text-zinc-600 dark:text-zinc-400 text-center max-w-xl">
            <div className="font-mono text-zinc-800 dark:text-zinc-300 truncate" title={name}>
                {name}
            </div>
            <div className="mt-1 flex flex-wrap items-center justify-center gap-x-3 gap-y-1">
                {logical !== null && (
                    <span>
                        <span className="text-zinc-500">logical:</span>{' '}
                        <span className="text-zinc-800 dark:text-zinc-200">
                            {fmtBytes(logical)}
                        </span>
                    </span>
                )}
                {onDiskSize !== null && (
                    <span>
                        <span className="text-zinc-500">{compressed ? 'on disk:' : 'size:'}</span>{' '}
                        <span className="text-zinc-800 dark:text-zinc-200">
                            {fmtBytes(onDiskSize)}
                        </span>
                    </span>
                )}
                {meta && (
                    <span>
                        <span className="text-zinc-500">format:</span>{' '}
                        <span className="text-zinc-800 dark:text-zinc-200">
                            {ptLabel(meta.pt_type)}
                        </span>
                    </span>
                )}
            </div>
        </div>
    );
}
