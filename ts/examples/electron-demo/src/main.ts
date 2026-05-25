/*
 * Electron main process for anyfs reader.
 *
 * Loads the same React app that ships to the web (`vite-demo`). In dev it
 * points the BrowserWindow at the vite dev server (already serves COOP/COEP);
 * in prod it registers `anyfs://app/` and serves `vite-demo/dist/` with the
 * same headers injected, so SharedArrayBuffer + the streaming-download
 * service worker continue to work.
 */

import { app, BrowserWindow, protocol, net, ipcMain, dialog } from 'electron';
import { createWriteStream, existsSync, mkdirSync, unlink, type WriteStream } from 'node:fs';
import { homedir } from 'node:os';
import { join, extname, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Opt-in dev mode (set by `pnpm dev`). Packaged builds are always prod.
// Bare `electron .` defaults to prod so we don't accidentally try to load
// a non-existent vite server.
const isDev = !app.isPackaged && process.env.ELECTRON_DEV === '1';

// Resolve the renderer directory across three launch shapes:
//   1. Packaged `./anyfs-demo` binary  → process.resourcesPath/renderer
//   2. System `electron resources/app` → ../../renderer (sibling of app/ in
//      the packaged layout, found via __dirname = resources/app/dist)
//   3. Dev workspace                   → ../../vite-demo/dist
// First candidate with an index.html wins. Whichever path we pick, the wasm
// bundle and worker live under it (renderer/wasm/, renderer/anyfs-worker.js)
// so the wasm fallback keeps working as long as RENDERER_DIR is right.
function pickRendererDir(): string {
    const candidates = [
        app.isPackaged ? join(process.resourcesPath, 'renderer') : null,
        resolve(__dirname, '..', '..', 'renderer'),
        resolve(__dirname, '..', '..', 'vite-demo', 'dist'),
    ].filter((p): p is string => p !== null);
    for (const c of candidates) {
        if (existsSync(join(c, 'index.html'))) return c;
    }
    return candidates[0];
}
const RENDERER_DIR = pickRendererDir();

const COOP_COEP_HEADERS = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
};

const MIME: Record<string, string> = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.cjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.map': 'application/json; charset=utf-8',
    '.wasm': 'application/wasm',
    '.webmanifest': 'application/manifest+json',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.gif': 'image/gif',
    '.ico': 'image/x-icon',
    '.woff': 'font/woff',
    '.woff2': 'font/woff2',
    '.ttf': 'font/ttf',
    '.txt': 'text/plain; charset=utf-8',
    '.img': 'application/octet-stream',
};

function mimeFor(path: string): string {
    return MIME[extname(path).toLowerCase()] ?? 'application/octet-stream';
}

// Privileged scheme registration MUST happen before app.whenReady().
// `secure: true` qualifies the origin for service workers + SAB; `standard`
// enables relative URL resolution; `stream: true` lets us return streaming
// responses (we use Buffer for now but keep the door open).
protocol.registerSchemesAsPrivileged([
    {
        scheme: 'anyfs',
        privileges: {
            secure: true,
            standard: true,
            supportFetchAPI: true,
            corsEnabled: true,
            allowServiceWorkers: true,
            stream: true,
        },
    },
    {
        // Proxy scheme for URLFS reads. The renderer/worker rewrites
        // `https://example.com/foo.img` to
        // `anyfs-url://proxy/?u=<encoded>` so the actual network request
        // happens in the main process (via net.fetch), bypassing the
        // browser same-origin policy. Lets the demo open arbitrary disk
        // image URLs without the upstream needing to ship CORS headers.
        scheme: 'anyfs-url',
        privileges: {
            secure: true,
            standard: true,
            supportFetchAPI: true,
            corsEnabled: true,
            stream: true,
        },
    },
]);

function resolveRendererPath(urlPath: string): string | null {
    // Strip leading `/`. Default to index.html for the root path.
    let rel = decodeURIComponent(urlPath.replace(/^\/+/, ''));
    if (rel === '' || rel.endsWith('/')) rel = rel + 'index.html';
    // Normalize and reject any traversal outside RENDERER_DIR.
    const abs = normalize(join(RENDERER_DIR, rel));
    if (!abs.startsWith(RENDERER_DIR)) return null;
    return abs;
}

// CORS headers shared with the proxy handler. Origin is `anyfs://app` (or
// http://localhost:5173 in dev); either way the renderer treats the proxy
// scheme as cross-origin, so we need ACAO + ACEH so probeUrl can read
// Content-Length / Accept-Ranges off the response.
const PROXY_CORS_HEADERS = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Expose-Headers': '*',
    'Access-Control-Allow-Methods': 'GET, HEAD, OPTIONS',
    'Access-Control-Allow-Headers': 'Range, Accept',
    'Access-Control-Max-Age': '86400',
};

async function handleAnyfsUrlRequest(request: Request): Promise<Response> {
    if (request.method === 'OPTIONS') {
        return new Response(null, { status: 204, headers: PROXY_CORS_HEADERS });
    }
    const url = new URL(request.url);
    const target = url.searchParams.get('u');
    if (!target) {
        return new Response('anyfs-url: missing ?u=', {
            status: 400,
            headers: PROXY_CORS_HEADERS,
        });
    }
    if (!/^https?:\/\//i.test(target)) {
        return new Response('anyfs-url: only http(s) targets are allowed', {
            status: 400,
            headers: PROXY_CORS_HEADERS,
        });
    }
    // Forward only headers that affect the read; in particular Range. We
    // intentionally drop Origin / Referer so the upstream sees a plain
    // anonymous request — that's the whole point of going through main.
    const fwd = new Headers();
    const range = request.headers.get('range');
    if (range) fwd.set('Range', range);
    const accept = request.headers.get('accept');
    if (accept) fwd.set('Accept', accept);
    try {
        const upstream = await net.fetch(target, {
            method: request.method,
            headers: fwd,
            redirect: 'follow',
        });
        const headers = new Headers(upstream.headers);
        for (const [k, v] of Object.entries(PROXY_CORS_HEADERS)) headers.set(k, v);
        return new Response(upstream.body, { status: upstream.status, headers });
    } catch (err) {
        const msg = err instanceof Error ? err.message : String(err);
        return new Response(`anyfs-url: upstream fetch failed: ${msg}`, {
            status: 502,
            headers: PROXY_CORS_HEADERS,
        });
    }
}

async function handleAnyfsRequest(request: Request): Promise<Response> {
    const url = new URL(request.url);
    // anyfs://app/<path>  — host is "app", path is the asset relative to RENDERER_DIR.
    const abs = resolveRendererPath(url.pathname);
    if (!abs) {
        return new Response('forbidden', { status: 403, headers: COOP_COEP_HEADERS });
    }
    // Delegate the file read to Electron's net module so we get streaming +
    // proper Range handling for free (matters for large disk images served
    // from /disks/).
    const fileUrl = `file://${abs}`;
    try {
        const upstream = await net.fetch(fileUrl, {
            method: request.method,
            headers: request.headers,
        });
        const headers = new Headers(upstream.headers);
        for (const [k, v] of Object.entries(COOP_COEP_HEADERS)) headers.set(k, v);
        headers.set('Content-Type', mimeFor(abs));
        return new Response(upstream.body, { status: upstream.status, headers });
    } catch (err) {
        return new Response(`not found: ${url.pathname}`, {
            status: 404,
            headers: COOP_COEP_HEADERS,
        });
    }
}

function createWindow(): void {
    const win = new BrowserWindow({
        width: 1280,
        height: 800,
        backgroundColor: '#18181b',
        webPreferences: {
            preload: join(__dirname, 'preload.cjs'),
            contextIsolation: true,
            nodeIntegration: false,
            // sandbox:false is required so the preload can `require()` the
            // future N-API native module. Empty preload today is fine either
            // way; setting it now avoids a breaking change later.
            sandbox: false,
        },
    });

    if (isDev) {
        void win.loadURL('http://localhost:5173/');
        win.webContents.openDevTools({ mode: 'detach' });
    } else {
        void win.loadURL('anyfs://app/');
    }
}

// Streaming download IPC bridge. The renderer's stream-download.ts normally
// drives downloads via a Service Worker that returns Content-Disposition:
// attachment for a magic URL. That browser path crashes Electron in
// `-fno-exceptions` mode (bad_optional_access inside Chromium's download
// manager) whenever the response comes from a privileged custom protocol
// like ours. Going through ipc + fs.createWriteStream skips the browser
// download path entirely.
type Download = { stream: WriteStream; path: string };
const downloads = new Map<string, Download>();

function installDownloadIpc() {
    const downloadsDir = join(homedir(), 'Downloads');
    try {
        mkdirSync(downloadsDir, { recursive: true });
    } catch {}

    ipcMain.handle('download:open', async (event, fileName: string) => {
        const owner = BrowserWindow.fromWebContents(event.sender);
        const suggested = fileName || 'download.bin';
        const result = await (owner
            ? dialog.showSaveDialog(owner, { defaultPath: join(downloadsDir, suggested) })
            : dialog.showSaveDialog({ defaultPath: join(downloadsDir, suggested) }));
        if (result.canceled || !result.filePath) {
            return { cancelled: true as const };
        }
        const id = `dl-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
        const ws = createWriteStream(result.filePath);
        downloads.set(id, { stream: ws, path: result.filePath });
        return { id, path: result.filePath };
    });

    ipcMain.handle('download:write', (_event, id: string, chunk: Uint8Array) => {
        const dl = downloads.get(id);
        if (!dl) throw new Error(`download not found: ${id}`);
        return new Promise<void>((resolve, reject) => {
            const ok = dl.stream.write(
                Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength),
                (err) => {
                    if (err) reject(err);
                },
            );
            if (ok) resolve();
            else dl.stream.once('drain', () => resolve());
        });
    });

    ipcMain.handle('download:close', (_event, id: string) => {
        const dl = downloads.get(id);
        if (!dl) return;
        downloads.delete(id);
        return new Promise<void>((resolve, reject) => {
            dl.stream.end((err: NodeJS.ErrnoException | null | undefined) =>
                err ? reject(err) : resolve(),
            );
        });
    });

    ipcMain.handle('download:cancel', (_event, id: string) => {
        const dl = downloads.get(id);
        if (!dl) return;
        downloads.delete(id);
        dl.stream.destroy();
        unlink(dl.path, () => {});
    });
}

void app.whenReady().then(() => {
    if (!isDev) protocol.handle('anyfs', handleAnyfsRequest);
    // The url proxy must be live in dev too — the renderer's worker still
    // rewrites URLs through it when running against the vite dev server.
    protocol.handle('anyfs-url', handleAnyfsUrlRequest);
    installDownloadIpc();
    createWindow();

    app.on('activate', () => {
        // macOS: re-open a window when the dock icon is clicked.
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
});
