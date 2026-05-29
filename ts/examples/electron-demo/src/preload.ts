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

// Drive enumeration. The renderer's "system drives" panel uses this to
// list physical disks + partitions and surface their fstype/label so
// the user can pick one to mount. Open/raw-read isn't wired yet — this
// is the discovery half only.
const drives = {
    list: () => ipcRenderer.invoke('drives:list') as Promise<unknown[] | null>,
};
contextBridge.exposeInMainWorld('electronDrives', drives);

// Native "Open image…" file picker. The renderer uses this in native mode
// instead of <input type=file>: we want an absolute host path the native
// addon can attachPath against, not a File blob. Resolves to the picked
// path or null on cancel.
const electronDialog = {
    openImage: () => ipcRenderer.invoke('dialog:openImage') as Promise<string | null>,
};
contextBridge.exposeInMainWorld('electronDialog', electronDialog);

// Tell @anyfs/core that we can fetch http(s) URLs via the main process —
// the anyfs-url:// privileged scheme registered in main.ts is wired to
// net.fetch and bypasses the renderer's same-origin policy. Plain
// browsers don't set this, so URLFS keeps using direct XHR there.
contextBridge.exposeInMainWorld('__anyfs', {
    urlProxyPrefix: 'anyfs-url://proxy/?u=',
});

// Native @anyfs/native bridge — mirrors the addon's surface 1:1 so the
// renderer can drive the real LKL kernel running in the main process. When
// this object is present, @anyfs/core prefers it over the wasm path; absence
// falls back transparently. All ops are async because IPC is.
// Skip when ANYFS_DISABLE_NATIVE=1 — tests can force the wasm path.
if (!process.env.ANYFS_DISABLE_NATIVE) {
    const anyfsNative = {
        available: () => ipcRenderer.invoke('anyfs-native:available') as Promise<boolean>,
        init: (memMb: number, loglevel: number) =>
            ipcRenderer.invoke('anyfs-native:init', memMb, loglevel) as Promise<number>,
        diskOpen: (path: string, flags: number) =>
            ipcRenderer.invoke('anyfs-native:diskOpen', path, flags) as Promise<number>,
        registerUrl: (url: string) =>
            ipcRenderer.invoke('anyfs-native:registerUrl', url) as Promise<{
                proxyUrl: string;
                id: string;
            }>,
        unregisterUrl: (id: string) =>
            ipcRenderer.invoke('anyfs-native:unregisterUrl', id) as Promise<void>,
        diskClose: (h: number) =>
            ipcRenderer.invoke('anyfs-native:diskClose', h) as Promise<number>,
        diskListJson: (h: number) =>
            ipcRenderer.invoke('anyfs-native:diskListJson', h) as Promise<string>,
        diskMetaJson: (h: number) =>
            ipcRenderer.invoke('anyfs-native:diskMetaJson', h) as Promise<string>,
        diskEnter: (h: number, part: number, flags: number) =>
            ipcRenderer.invoke('anyfs-native:diskEnter', h, part, flags) as Promise<string>,
        mountWhole: (h: number, fstype: string, flags: number) =>
            ipcRenderer.invoke('anyfs-native:mountWhole', h, fstype, flags) as Promise<string>,
        readdirJson: (path: string) =>
            ipcRenderer.invoke('anyfs-native:readdirJson', path) as Promise<string>,
        lstatJson: (path: string) =>
            ipcRenderer.invoke('anyfs-native:lstatJson', path) as Promise<string>,
        statJson: (path: string) =>
            ipcRenderer.invoke('anyfs-native:statJson', path) as Promise<string>,
        realpath: (path: string) =>
            ipcRenderer.invoke('anyfs-native:realpath', path) as Promise<string>,
        readlink: (path: string) =>
            ipcRenderer.invoke('anyfs-native:readlink', path) as Promise<string>,
        fileOpen: (path: string, flags: number) =>
            ipcRenderer.invoke('anyfs-native:fileOpen', path, flags) as Promise<number>,
        pread: (fd: number, n: number, off: number) =>
            ipcRenderer.invoke('anyfs-native:pread', fd, n, off) as Promise<{
                rc: number;
                data: Uint8Array;
            }>,
        fileClose: (fd: number) =>
            ipcRenderer.invoke('anyfs-native:fileClose', fd) as Promise<number>,
    };
    contextBridge.exposeInMainWorld('anyfsNative', anyfsNative);
}
