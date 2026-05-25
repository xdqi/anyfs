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

// Tell @anyfs/core that we can fetch http(s) URLs via the main process —
// the anyfs-url:// privileged scheme registered in main.ts is wired to
// net.fetch and bypasses the renderer's same-origin policy. Plain
// browsers don't set this, so URLFS keeps using direct XHR there.
contextBridge.exposeInMainWorld('__anyfs', {
    urlProxyPrefix: 'anyfs-url://proxy/?u=',
});
