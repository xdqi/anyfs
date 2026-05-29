import { useCallback, useState } from 'react';
import type { AnyfsSession, NativeSession } from '@anyfs/core';
import { AnyfsFileBrowser } from '@anyfs/trees';
import { useSettings } from '../Settings';
import { streamDownload } from '../stream-download';
import { DownloadStatus } from './DownloadStatus';

export function DownloadingFileTree({
    disk,
    mountPath,
    rootLabel,
}: {
    disk: AnyfsSession | NativeSession;
    mountPath: string;
    rootLabel: string;
}) {
    // In-app progress + cancel UI is only meaningful under Electron, where the
    // download is piped straight to ~/Downloads via the bridge and there's no
    // browser-native download manager. In a real browser the SW path triggers
    // an attachment response and the browser's own UI takes over — showing
    // ours on top is duplicate, and worse, our onProgress reflects how fast
    // we feed chunks to the SW, not the disk write, so the bar can sit at 0%
    // while the browser is mid-download.
    const inElectron = typeof window !== 'undefined' && !!window.electronDownload;
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
                ...(inElectron
                    ? {
                          onProgress: (written: number) => {
                              job.written = written;
                              setActive({ ...job });
                          },
                      }
                    : {}),
            });
            job.cancel = () => handle.cancel();
            if (inElectron) setActive(job);
            try {
                await handle.promise;
                if (inElectron) setActive(null);
            } catch (err) {
                if (inElectron) setActive({ ...job, error: (err as Error).message });
                else console.error('[anyfs] download failed:', err);
            }
        },
        [disk, mountPath, inElectron, settings],
    );

    return (
        <>
            <AnyfsFileBrowser
                session={disk as AnyfsSession}
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

export interface DownloadJob {
    name: string;
    size: number;
    written: number;
    cancel: () => void;
    error?: string;
}
