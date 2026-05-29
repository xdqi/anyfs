import { useCallback, useEffect, useMemo, useState } from 'react';
import type { DragEvent as ReactDragEvent } from 'react';
import type { SessionSource } from '@anyfs/core';
import { getAnyfsNative, getUrlProxyPrefix } from '@anyfs/core';
import { useSettings } from '../Settings';
import {
    addRecentFile,
    listRecents,
    removeRecent,
    tryReopen,
    fsaSupported,
    pickFile,
    type Recent,
} from '../recents';

function FilePicker({ onSource }: { onSource: (s: SessionSource) => void }) {
    const [dragging, setDragging] = useState(false);
    const [errorDialog, setErrorDialog] = useState<{ title: string; message: string } | null>(null);
    const [recents, setRecents] = useState<Recent[]>([]);
    // null = no overlay; else which action dialog is open.
    const [openDialog, setOpenDialog] = useState<'url' | 'drives' | null>(null);
    const fsa = fsaSupported();
    // Decide at render time. Native (Electron) trades the in-browser File
    // path for absolute host paths handed straight to the LKL kernel — that
    // lets us mount things the browser sandbox can't see (system drives,
    // big disk images on disk) and unlocks the QEMU curl URL path.
    const { settings } = useSettings();
    const nativeMode = !!getAnyfsNative() && !settings.disableNative;
    const electronDialog = nativeMode ? getElectronDialog() : null;
    const electronDrives = nativeMode ? getElectronDrives() : null;

    const refreshRecents = useCallback(async () => {
        setRecents(await listRecents());
    }, []);
    useEffect(() => {
        void refreshRecents();
    }, [refreshRecents]);

    // Always-on path: produce a SessionSource and stash a recents entry when we
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

    // Try to lift a FileSystemFileHandle out of a drop event so dropped files
    // also become recents on Chrome/Edge. Falls back to dataTransfer.files
    // (still mounts, just doesn't persist).
    const onDrop = async (e: ReactDragEvent) => {
        e.preventDefault();
        setDragging(false);
        if (nativeMode) {
            // Native side has no File→path bridge; ask the user to use the
            // dialog instead so we get an absolute host path.
            setErrorDialog({
                title: 'Drag-and-drop not supported in native mode',
                message:
                    'Use “Open file…” to pick a disk image — the native bridge needs an absolute host path, not an in-browser File object.',
            });
            return;
        }
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

    // Native picker. In Electron we always go through dialog:openImage so we
    // get an absolute host path the LKL kernel can attach. In browsers we
    // prefer FSA (gives a persistable handle for Recents) and fall back to
    // the hidden <input id="anyfs-legacy-file">.
    const onOpenFile = async () => {
        try {
            if (electronDialog) {
                const p = await electronDialog.openImage();
                if (!p) return;
                const name = sourceName({ kind: 'path', path: p });
                try {
                    await addRecentPath(p, name);
                } catch {}
                void refreshRecents();
                onSource({ kind: 'path', path: p, name });
                return;
            }
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

    // URL dialog submit. In native mode the HTTP proxy server translates
    // QEMU Range requests into upstream HTTPS fetches; the source stays
    // `kind:'url'` so the provider routes through attachUrl.
    const onSubmitUrl = async (trimmed: string) => {
        setOpenDialog(null);
        if (nativeMode) {
            const name = sourceName({ kind: 'url', url: trimmed });
            try {
                await addRecentUrl(trimmed, name);
            } catch {}
            void refreshRecents();
            onSource({ kind: 'url', url: trimmed, name });
            return;
        }
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
        }
    };

    // System-drives picker. Only available in native mode.
    const onPickDrive = async (path: string, name: string) => {
        setOpenDialog(null);
        try {
            await addRecentPath(path, name);
        } catch {}
        void refreshRecents();
        onSource({ kind: 'path', path, name });
    };

    const onReopen = async (r: Recent) => {
        try {
            const res = await tryReopen(r);
            if (res.kind === 'ok') {
                let src = res.source;
                if (src.kind === 'url') {
                    if (nativeMode) {
                        // Native HTTP proxy server handles the URL — keep as
                        // kind:'url' so the provider routes to attachUrl.
                    } else {
                        // Wasm/URLFS path: re-probe so CORS/404 surfaces here
                        // rather than as a worker crash.
                        try {
                            await probeUrlAhead(src.url);
                        } catch (e) {
                            setErrorDialog({
                                title: 'Can’t reopen URL',
                                message: (e as Error).message,
                            });
                            return;
                        }
                    }
                } else if (src.kind === 'file' && nativeMode) {
                    // We're in native mode but the recent is a browser File
                    // (saved before the user switched to Electron, or by a
                    // mixed build). Force-fail rather than letting the worker
                    // path try to attach a File the native bridge rejects.
                    setErrorDialog({
                        title: 'Can’t reopen this file in native mode',
                        message:
                            'This recent was saved as an in-browser File handle. Use “Open file…” to pick it again by path.',
                    });
                    return;
                }
                await refreshRecents();
                onSource(src);
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

    return (
        <div
            className="flex-1 flex items-center justify-center p-6"
            onDragEnter={(e) => {
                if (nativeMode) return;
                e.preventDefault();
                setDragging(true);
            }}
            onDragOver={(e) => {
                if (nativeMode) return;
                e.preventDefault();
                setDragging(true);
            }}
            onDragLeave={() => setDragging(false)}
            onDrop={(e) => {
                void onDrop(e);
            }}
        >
            <div
                className={[
                    'w-full max-w-xl rounded-2xl bg-white border dark:bg-zinc-900 p-6 space-y-5 shadow-xl transition-colors',
                    dragging
                        ? 'border-emerald-500 ring-2 ring-emerald-500/40'
                        : 'border-zinc-200 dark:border-zinc-800',
                ].join(' ')}
            >
                {/* Always in DOM so CDP tests can inject files via setFileInputFiles.
                    FSA is the primary path when available; this is the fallback. */}
                <input
                    id="anyfs-legacy-file"
                    type="file"
                    className="hidden"
                    onChange={(e) => {
                        const f = e.target.files?.[0];
                        if (f) void acceptFile(f);
                    }}
                />
                <div className="space-y-1">
                    <h1 className="text-2xl font-semibold text-zinc-900 dark:text-zinc-100">
                        anyfs reader
                    </h1>
                    <p className="text-sm text-zinc-500">
                        {nativeMode
                            ? 'Native bridge active — host paths and URLs go through QEMU + LKL in the main process.'
                            : 'Browser bundle — files, URL Range fetches, and Origin Private FS only.'}
                    </p>
                </div>

                {recents.length > 0 && (
                    <RecentsList recents={recents} onReopen={onReopen} onRemove={onRemove} />
                )}

                <div className="space-y-2">
                    <div className="text-xs uppercase tracking-wider text-zinc-500">Start</div>
                    <ul className="space-y-1">
                        <li>
                            <button
                                type="button"
                                onClick={() => void onOpenFile()}
                                className="text-emerald-700 dark:text-emerald-400 hover:underline text-base"
                            >
                                Open file…
                            </button>
                            <span className="text-sm text-zinc-500 ml-2">
                                {nativeMode
                                    ? 'pick a disk image by absolute path'
                                    : 'pick a local disk image'}
                            </span>
                        </li>
                        <li>
                            <button
                                type="button"
                                onClick={() => setOpenDialog('url')}
                                className="text-emerald-700 dark:text-emerald-400 hover:underline text-base"
                            >
                                Open URL…
                            </button>
                            <span className="text-sm text-zinc-500 ml-2">
                                {nativeMode
                                    ? 'http(s) / ftp / sftp via QEMU curl'
                                    : 'http(s) with Range requests'}
                            </span>
                        </li>
                        {electronDrives && (
                            <li>
                                <button
                                    type="button"
                                    onClick={() => setOpenDialog('drives')}
                                    className="text-emerald-700 dark:text-emerald-400 hover:underline text-base"
                                >
                                    Open system drive…
                                </button>
                                <span className="text-sm text-zinc-500 ml-2">
                                    physical disks and partitions
                                </span>
                            </li>
                        )}
                    </ul>
                </div>

                {!nativeMode && (
                    <p className="text-xs text-zinc-500 leading-relaxed">
                        Tip — drop a disk image anywhere on this card to open it.
                    </p>
                )}

                <SupportedFormats />
            </div>
            {openDialog === 'url' && (
                <UrlPromptDialog
                    mode={nativeMode ? 'native' : 'wasm'}
                    onSubmit={(u) => {
                        void onSubmitUrl(u);
                    }}
                    onClose={() => setOpenDialog(null)}
                />
            )}
            {openDialog === 'drives' && (
                <SystemDrivesDialog onPick={onPickDrive} onClose={() => setOpenDialog(null)} />
            )}
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
