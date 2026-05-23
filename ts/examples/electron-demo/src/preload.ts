/*
 * Preload bridge.
 *
 * Exposes `window.electronDownload` — a streaming-write IPC bridge used to
 * bypass the renderer's Service-Worker + Content-Disposition: attachment
 * download trick. That trick crashes Electron (`bad_optional_access` in
 * Chromium's download-manager code path) when the response comes from a
 * privileged custom protocol like our `anyfs://`. Going through main lets us
 * write the bytes directly to ~/Downloads without involving the browser
 * download manager at all.
 *
 * stream-download.ts feature-detects this object and falls back to the SW
 * path in plain browsers, so the same renderer code ships to both targets.
 */

import { contextBridge, ipcRenderer } from 'electron';

const api = {
    open: (fileName: string, size: number | null) =>
        ipcRenderer.invoke('download:open', fileName, size) as Promise<{
            id: string;
            path: string;
        }>,
    write: (id: string, chunk: Uint8Array) =>
        ipcRenderer.invoke('download:write', id, chunk) as Promise<void>,
    close: (id: string) => ipcRenderer.invoke('download:close', id) as Promise<void>,
    cancel: (id: string) => ipcRenderer.invoke('download:cancel', id) as Promise<void>,
};

contextBridge.exposeInMainWorld('electronDownload', api);
