import { useCallback, useEffect, useState } from 'react';
import type { SessionSource } from '@anyfs/core';
import { getAnyfsNative } from '@anyfs/core';
import { AnyfsProvider } from '@anyfs/react';
import { SettingsDialog, SettingsProvider } from './Settings';
import { TopBar } from './components/TopBar';
import { ConfirmDialog } from './components/ConfirmDialog';
import { AboutDialog } from './components/AboutDialog';
import { DiskView } from './components/DiskView';
import { KernelStatusBar } from './components/KernelStatusBar';
import { FilePicker } from './components/FilePicker';

// The wasm lives inside a dedicated Web Worker (required: WORKERFS asserts
// ENVIRONMENT_IS_WORKER, and LKL sem_wait → Atomics.wait is blocked on
// Chrome's main thread). The worker script and wasm bundle are served from
// /public/wasm/.

import { TopBar } from './components/TopBar';
import { ConfirmDialog } from './components/ConfirmDialog';
import { AboutDialog } from './components/AboutDialog';
import { SystemDrivesDialog } from './components/SystemDrivesDialog';
import { UrlPromptDialog } from './components/UrlPromptDialog';
import { FilePicker } from './components/FilePicker';
import { UrlErrorDialog } from './components/UrlErrorDialog';
import { RecentsList } from './components/RecentsList';
import { KernelStatusBar } from './components/KernelStatusBar';
import { DiskView } from './components/DiskView';
import { DownloadStatus, DownloadJob } from './components/DownloadStatus';

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

// User-facing label for a SessionSource. For files it's the on-disk filename;
// for URLs we lift the last path segment (decoded), falling back to host or
// the raw URL if the path is empty.
function sourceName(s: SessionSource): string {
    if (s.kind === 'file') return s.file.name;
    if (s.kind === 'path') {
        if (s.name) return s.name;
        // Best-effort basename: take the last segment after / or \.
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

interface ConfirmCfg {
    title: string;
    message: string;
    confirmLabel: string;
    onConfirm: () => void;
}

export function App() {
    const [source, setSource] = useState<SessionSource | null>(null);

    // CDP test hook — exposes openUrl / openPath so headless tests can
    // drive disk mounting without fighting React synthetic events.
    useEffect(() => {
        const api = {
            openUrl: (url: string) => {
                console.log('[APP] openUrl called with', url);
                const name = sourceName({ kind: 'url', url });
                console.log('[APP] openUrl setting source name=', name);
                setSource({ kind: 'url', url, name });
            },
            openPath: (path: string) => {
                const name = sourceName({ kind: 'path', path });
                setSource({ kind: 'path', path, name });
            },
            /** Directly set source to a file — CDP setFileInputFiles trigger. */
            setSourceFile: (file: File) => {
                setSource({ kind: 'file', file });
            },
        };
        (window as any).__anyfsTest = api;
        return () => {
            delete (window as any).__anyfsTest;
        };
    }, []);
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

    // Read disableNative from localStorage so we can feed it as forceMode to
    // AnyfsProvider before SettingsProvider mounts (useSettings isn't available
    // until SettingsProvider is in the tree).
    const settingsDisableNative = (() => {
        try {
            if (typeof localStorage !== 'undefined') {
                const raw = localStorage.getItem('anyfs.settings.v1');
                if (raw) return !!(JSON.parse(raw) as Record<string, unknown>).disableNative;
            }
        } catch {}
        return false;
    })();

    return (
        <SettingsProvider>
            <AnyfsProvider
                source={source}
                workerUrl={WORKER_URL}
                wasmBaseUrl="/wasm/"
                wasmModuleName="anyfs.qemu.mjs"
                autoMount="auto"
                mountOpts={{ loglevel: 7 }}
                prewarm
                {...(settingsDisableNative ? { forceMode: 'wasm' as const } : {})}
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
                    <SettingsDialog
                        open={settingsOpen}
                        onClose={() => setSettingsOpen(false)}
                        nativeAvailable={!!getAnyfsNative()}
                    />
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
