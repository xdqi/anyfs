// Client side of the streaming-download bridge. See public/sw-download.js for
// the SW half. Triggers a native browser download (no Save As dialog) for
// data we produce on the fly.
//
// Under Electron the SW + Content-Disposition: attachment + iframe trick
// crashes Chromium's download manager when the response comes from our
// privileged anyfs:// protocol (bad_optional_access in -fno-exceptions mode).
// The Electron preload exposes `window.electronDownload` so we can pipe the
// stream straight to ~/Downloads via main-process fs writes. That bridge is
// absent in plain browsers, where the SW path is the right answer.

interface ElectronDownloadBridge {
    open: (fileName: string, size: number | null) => Promise<{ id: string; path: string }>;
    write: (id: string, chunk: Uint8Array) => Promise<void>;
    close: (id: string) => Promise<void>;
    cancel: (id: string) => Promise<void>;
}

declare global {
    interface Window {
        electronDownload?: ElectronDownloadBridge;
    }
}

let swReady: Promise<ServiceWorker> | null = null;

export function ensureDownloadServiceWorker(): Promise<ServiceWorker> {
    if (swReady) return swReady;
    if (!('serviceWorker' in navigator)) {
        return Promise.reject(new Error('Service Workers not supported'));
    }
    swReady = (async () => {
        const reg = await navigator.serviceWorker.register('/sw-download.js', { scope: '/' });
        const sw = reg.active ?? reg.waiting ?? reg.installing;
        if (!sw) throw new Error('Service Worker failed to register');
        if (sw.state !== 'activated') {
            await new Promise<void>((resolve) => {
                sw.addEventListener('statechange', function onState() {
                    if (sw.state === 'activated') {
                        sw.removeEventListener('statechange', onState);
                        resolve();
                    }
                });
            });
        }
        // Don't return `sw` (the original worker may have been replaced on
        // update). The controller is what will actually serve our fetch.
        if (navigator.serviceWorker.controller) {
            return navigator.serviceWorker.controller;
        }
        await new Promise<void>((resolve) => {
            navigator.serviceWorker.addEventListener('controllerchange', () => resolve(), {
                once: true,
            });
        });
        return navigator.serviceWorker.controller!;
    })();
    return swReady;
}

export interface StreamDownloadOpts {
    stream: ReadableStream<Uint8Array>;
    fileName: string;
    /** Total size if known. Sent as Content-Length so the browser shows a real progress bar. */
    size?: number;
    onProgress?: (written: number) => void;
}

export interface StreamDownloadHandle {
    /** Resolves once all bytes are flushed to the SW (and therefore queued in the download). */
    promise: Promise<void>;
    /** Cancel the upstream stream and tell the SW to error the response. */
    cancel: (reason?: string) => void;
}

function electronStreamDownload(opts: StreamDownloadOpts): StreamDownloadHandle {
    const bridge = window.electronDownload!;
    const { stream, fileName, size, onProgress } = opts;
    let cancelFn = (_r?: string) => {};

    const promise = (async () => {
        const opened = await bridge.open(fileName, size ?? null);
        if ('cancelled' in opened) {
            // User dismissed the Save As dialog. Resolve silently — no error,
            // no progress, no side effects. Caller will clear UI state via
            // its existing setActive(null)-on-success branch.
            return;
        }
        const { id } = opened;

        const reader = stream.getReader();
        let cancelled = false;
        let written = 0;
        cancelFn = (reason?: string) => {
            cancelled = true;
            try {
                void reader.cancel(reason ?? 'user-cancelled');
            } catch {}
            void bridge.cancel(id).catch(() => {});
        };

        try {
            for (;;) {
                const { value, done } = await reader.read();
                if (cancelled || done) break;
                // Chunks may be backed by SharedArrayBuffer; the structured-
                // clone path from contextBridge won't transfer SAB views, so
                // copy into a fresh ArrayBuffer.
                const copy = new Uint8Array(value.byteLength);
                copy.set(value);
                try {
                    await bridge.write(id, copy);
                } catch (err) {
                    if (cancelled) break; // bridge.cancel already cleaned up
                    throw err;
                }
                written += value.byteLength;
                onProgress?.(written);
            }
            if (!cancelled) await bridge.close(id);
        } catch (err) {
            void bridge.cancel(id).catch(() => {});
            if (cancelled) return; // user pressed cancel — swallow silently
            throw err;
        }
    })();

    return {
        promise,
        cancel: (reason?: string) => cancelFn(reason),
    };
}

function swStreamDownload(opts: StreamDownloadOpts): StreamDownloadHandle {
    const { stream, fileName, size, onProgress } = opts;
    let cancelFn = (_r?: string) => {};

    const promise = (async () => {
        const sw = await ensureDownloadServiceWorker();
        const token = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
        const path = `/__streamsave/${token}`;
        const channel = new MessageChannel();
        const port = channel.port1;

        sw.postMessage(
            { kind: 'streamsave:register', url: path, fileName, size, port: channel.port2 },
            [channel.port2],
        );

        // Hidden iframe navigates to the magic URL → SW intercepts → browser
        // sees Content-Disposition: attachment → download bar appears.
        const iframe = document.createElement('iframe');
        iframe.hidden = true;
        iframe.src = path;
        document.body.appendChild(iframe);

        const reader = stream.getReader();
        let cancelled = false;
        let written = 0;
        cancelFn = (reason?: string) => {
            cancelled = true;
            try {
                void reader.cancel(reason ?? 'user-cancelled');
            } catch {}
            try {
                port.postMessage({ kind: 'abort', reason: reason ?? 'user-cancelled' });
            } catch {}
        };

        // SW can send {kind:'cancel'} if the browser kills the download.
        port.onmessage = (ev) => {
            const m = ev.data;
            if (m && m.kind === 'cancel') cancelFn(m.reason);
        };

        try {
            for (;;) {
                const { value, done } = await reader.read();
                if (cancelled) break;
                if (done) break;
                // Chunks may be backed by SharedArrayBuffer (from the wasm
                // worker). MessagePort.postMessage refuses to transfer SAB
                // views, so copy into a fresh ArrayBuffer.
                const copy = new Uint8Array(value.byteLength);
                copy.set(value);
                port.postMessage({ kind: 'chunk', data: copy }, [copy.buffer]);
                written += value.byteLength;
                onProgress?.(written);
            }
            if (!cancelled) port.postMessage({ kind: 'end' });
        } catch (err) {
            try {
                port.postMessage({ kind: 'abort', reason: (err as Error).message });
            } catch {}
            throw err;
        } finally {
            // Give the SW a moment to finish flushing and the browser a
            // moment to commit the download before tearing the iframe down.
            setTimeout(() => {
                try {
                    port.close();
                } catch {}
                try {
                    iframe.remove();
                } catch {}
            }, 5000);
        }
    })();

    return {
        promise,
        cancel: (reason?: string) => cancelFn(reason),
    };
}

export function streamDownload(opts: StreamDownloadOpts): StreamDownloadHandle {
    if (typeof window !== 'undefined' && window.electronDownload) {
        return electronStreamDownload(opts);
    }
    return swStreamDownload(opts);
}
