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
import type { Drive } from 'drivelist';
import { Worker } from 'node:worker_threads';
import { loadAnyfsNativeAddon, loadDrivelistModule } from './native-loader';

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

// Native "Open Image…" file picker. Renderer calls this instead of the FSA
// <input type=file> when the native bridge is in effect — we want an
// absolute host path string (which the native addon can attachPath), not a
// File blob (which the addon can't consume). Returns the picked path or
// null if the user cancelled.
function installDialogIpc() {
    ipcMain.handle('dialog:openImage', async (event) => {
        // Test hook: skip the native OS dialog when a path is pre-set.
        // Native file dialogs can't be automated via CDP; this lets tests
        // exercise the full IPC + addon pipeline without manual interaction.
        if (process.env.ANYFS_TEST_LOCAL_PATH) return process.env.ANYFS_TEST_LOCAL_PATH;
        const owner = BrowserWindow.fromWebContents(event.sender);
        const result = await (owner
            ? dialog.showOpenDialog(owner, {
                  title: 'Open disk image',
                  properties: ['openFile'],
                  filters: [
                      {
                          name: 'Disk images',
                          extensions: [
                              'img',
                              'iso',
                              'raw',
                              'bin',
                              'qcow2',
                              'qcow',
                              'vmdk',
                              'vdi',
                              'vhd',
                              'vhdx',
                              'dmg',
                              'vpc',
                          ],
                      },
                      { name: 'All files', extensions: ['*'] },
                  ],
              })
            : dialog.showOpenDialog({
                  title: 'Open disk image',
                  properties: ['openFile'],
              }));
        if (result.canceled || result.filePaths.length === 0) return null;
        return result.filePaths[0];
    });
}

// System-drive enumeration via drivelist-anyfs. Returns the full Drive[] —
// renderer decides what to show. Errors come back as null so the UI can
// fall back to "no drives" rather than blowing up the picker. This is
// strictly read-only at this stage: the renderer can SEE devices and
// partitions but cannot yet OPEN them through Electron (that needs raw
// device access + an IPC reader stream, future work).
function installDrivesIpc() {
    ipcMain.handle('drives:list', async () => {
        try {
            const drivelist = loadDrivelistModule();
            const drives: Drive[] = await drivelist.list();
            return drives;
        } catch (e) {
            console.error('[drives:list] failed:', e);
            return null;
        }
    });
}

// Native @anyfs/native bridge. The addon is one process-global LKL kernel —
// `init(mem_mb, loglevel)` brings it up exactly once, and every subsequent
// handle/path op runs against the same kernel. We lazy-require the addon so
// the demo still launches if the .node is missing for the current platform
// (e.g. dev on a host without it built) — renderer feature-detects via
// `window.anyfsNative` and falls back to the wasm path.
// Mirrors the N-API exports of anyfs_native.node. These were renamed in the
// session-API refactor (init→kernelInit, disk*→session*, mountWhole deleted —
// whole-disk mounting is now sessionEnter(h, 0, flags)); keep this in lockstep
// with packages/anyfs-native/src/binding.cc or every IPC handler below calls
// an undefined function.
type AnyfsNativeModule = {
    kernelInit(memMb: number, loglevel: number): number;
    kernelHalt(): number;
    sessionOpen(imagePath: string, flags: number): number;
    sessionClose(h: number): number;
    sessionListJson(h: number): string;
    sessionMetaJson(h: number): string;
    sessionEnter(h: number, part: number, flags: number): string;
    readdirJson(path: string): string;
    lstatJson(path: string): string;
    statJson(path: string): string;
    realpath(path: string): string;
    readlink(path: string): string;
    fileOpen(path: string, flags: number): number;
    pread(fd: number, buf: Uint8Array, n: number, off: number): number;
    fileClose(fd: number): number;
};

let nativeMod: AnyfsNativeModule | null = null;
let nativeInitDone = false;

// Per-disk HTTP proxy workers. Each disk gets its own Worker thread running
// an HTTP server on a random port. The addon calls block the calling thread's
// event loop, so the proxy server MUST live in a separate thread.
// This also anticipates the privileged-process model: later that Worker can
// be replaced by a child process exposing \\.\PhysicalDriveN.
const diskProxies = new Map<string, { worker: Worker; port: number }>();

function loadNativeAddon(): AnyfsNativeModule | null {
    if (nativeMod) return nativeMod;
    try {
        const m = loadAnyfsNativeAddon() as AnyfsNativeModule | null;
        if (!m) throw new Error('anyfs_native.node not found at any staged path');
        nativeMod = m;
        return m;
    } catch (e) {
        console.warn('[anyfs-native] addon not loadable:', (e as Error).message);
        return null;
    }
}

function installAnyfsNativeIpc() {
    // Probe at startup so the renderer's feature-detect (`anyfs-native:available`)
    // is cheap and synchronous from its POV.
    const probe = loadNativeAddon();
    if (!probe) {
        console.log('[anyfs-native] addon unavailable — renderer will use wasm path');
    } else {
        console.log('[anyfs-native] addon loaded; awaiting init from renderer');
    }

    ipcMain.handle('anyfs-native:available', () => loadNativeAddon() !== null);

    ipcMain.handle('anyfs-native:init', (_event, memMb: number, loglevel: number) => {
        const m = loadNativeAddon();
        if (!m) throw new Error('anyfs-native addon not loadable');
        if (nativeInitDone) return 0; // idempotent — kernel is global
        const rc = m.kernelInit(memMb >>> 0, loglevel >>> 0);
        if (rc === 0) nativeInitDone = true;
        return rc;
    });

    ipcMain.handle('anyfs-native:diskOpen', (_event, path: string, flags: number) => {
        const m = loadNativeAddon()!;
        return m.sessionOpen(path, flags >>> 0);
    });

    // Unified per-disk HTTP proxy: each disk gets its own Worker thread
    // running an HTTP server on a random port. Handles both remote URLs
    // (fetch + proxy) and local file paths (fs.createReadStream + Range).
    // The addon calls block the main thread's event loop, so the proxy MUST
    // live on a separate thread with its own libuv loop.
    ipcMain.handle(
        'anyfs-native:startProxy',
        async (_event, payload: { upstreamUrl?: string; localPath?: string }) => {
            const workerPath = join(__dirname, 'http-proxy-worker.cjs');
            const label = payload.upstreamUrl ?? payload.localPath ?? '?';
            console.log(`[anyfs-native] spawning proxy worker for ${label}`);
            const worker = new Worker(workerPath, {
                workerData: payload,
            });
            worker.on('exit', (code) => {
                console.log(`[anyfs-native] proxy worker exited with code=${code}`);
            });
            worker.on('error', (err) => {
                console.error(`[anyfs-native] proxy worker error: ${err.message}`);
            });
            const result = await new Promise<{ port: number } | { error: string }>(
                (resolve, reject) => {
                    const timer = setTimeout(() => {
                        reject(new Error('proxy worker startup timed out after 20s'));
                    }, 20000);
                    worker.once('message', (msg) => {
                        clearTimeout(timer);
                        resolve(msg);
                    });
                    worker.once('error', (err) => {
                        clearTimeout(timer);
                        reject(err);
                    });
                },
            );
            if ('error' in result) {
                throw new Error(`proxy worker failed: ${result.error}`);
            }
            const { port } = result;
            console.log(`[anyfs-native] proxy worker listening on 127.0.0.1:${port}`);
            const id = `proxy-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
            diskProxies.set(id, { worker, port });
            return { proxyUrl: `http://127.0.0.1:${port}/`, id };
        },
    );
    ipcMain.handle('anyfs-native:stopProxy', async (_event, id: string) => {
        const entry = diskProxies.get(id);
        if (entry) {
            entry.worker.postMessage('stop');
            diskProxies.delete(id);
        }
    });
    ipcMain.handle('anyfs-native:diskClose', (_event, h: number) =>
        loadNativeAddon()!.sessionClose(h),
    );
    ipcMain.handle('anyfs-native:diskListJson', (_event, h: number) =>
        loadNativeAddon()!.sessionListJson(h),
    );
    ipcMain.handle('anyfs-native:diskMetaJson', (_event, h: number) =>
        loadNativeAddon()!.sessionMetaJson(h),
    );
    ipcMain.handle('anyfs-native:diskEnter', (_event, h: number, part: number, flags: number) =>
        loadNativeAddon()!.sessionEnter(h, part >>> 0, flags >>> 0),
    );
    // `mountWhole` was deleted in the session-API refactor — whole-disk
    // mounting is now diskEnter(h, 0, flags). The preload still exposes a
    // mountWhole bridge method for back-compat, but nothing in the renderer
    // calls it, so no IPC handler is registered here.

    ipcMain.handle('anyfs-native:readdirJson', (_event, path: string) =>
        loadNativeAddon()!.readdirJson(path),
    );
    ipcMain.handle('anyfs-native:lstatJson', (_event, path: string) =>
        loadNativeAddon()!.lstatJson(path),
    );
    ipcMain.handle('anyfs-native:statJson', (_event, path: string) =>
        loadNativeAddon()!.statJson(path),
    );
    ipcMain.handle('anyfs-native:realpath', (_event, path: string) =>
        loadNativeAddon()!.realpath(path),
    );
    ipcMain.handle('anyfs-native:readlink', (_event, path: string) =>
        loadNativeAddon()!.readlink(path),
    );

    ipcMain.handle('anyfs-native:fileOpen', (_event, path: string, flags: number) =>
        loadNativeAddon()!.fileOpen(path, flags | 0),
    );
    // pread returns the populated Uint8Array slice — IPC structured-cloning a
    // Uint8Array is fine, and avoids the renderer having to send a buffer of
    // its own for the kernel to fill. `got` may be < n on short reads or EOF.
    ipcMain.handle('anyfs-native:pread', (_event, fd: number, n: number, off: number) => {
        const m = loadNativeAddon()!;
        const buf = new Uint8Array(n >>> 0);
        const got = m.pread(fd, buf, n >>> 0, off);
        if (got < 0) return { rc: got, data: new Uint8Array(0) };
        return { rc: got, data: got === buf.length ? buf : buf.subarray(0, got) };
    });
    ipcMain.handle('anyfs-native:fileClose', (_event, fd: number) =>
        loadNativeAddon()!.fileClose(fd),
    );
}

void app.whenReady().then(async () => {
    // Headless smoke path: dump drives to a file and exit. Lets CI/devs
    // confirm the drivelist binding is callable from this electron build
    // without the GUI/wasm prewarm path.
    if (process.env.ANYFS_DRIVES_SMOKE === '1') {
        try {
            const drives = await loadDrivelistModule().list();
            const out = process.env.ANYFS_DRIVES_OUT || '/tmp/anyfs-drives-smoke.json';
            const fs = await import('node:fs/promises');
            await fs.writeFile(out, JSON.stringify(drives, null, 2));
            console.log(`[drives:smoke] wrote ${drives.length} drives to ${out}`);
        } catch (e) {
            console.error('[drives:smoke] failed:', e);
        }
        app.exit(0);
        return;
    }

    // Same headless pattern for the native addon: init kernel, open the image
    // passed in $ANYFS_NATIVE_IMAGE, dump diskListJson, exit. Lets us confirm
    // the addon loads under Electron's vendored Node ABI before the renderer
    // ever touches it.
    if (process.env.ANYFS_NATIVE_SMOKE === '1') {
        try {
            const m = loadNativeAddon();
            if (!m) throw new Error('addon not loadable');
            const rc = m.kernelInit(512, 4);
            if (rc !== 0) throw new Error(`init rc=${rc}`);
            nativeInitDone = true;
            const img =
                process.env.ANYFS_NATIVE_IMAGE ||
                resolve(__dirname, '../../vite-demo/public/disks/multi.img');
            const h = m.sessionOpen(img, 0);
            if (h < 0) throw new Error(`diskOpen rc=${h}`);
            const list = m.sessionListJson(h);
            const out = process.env.ANYFS_NATIVE_OUT || '/tmp/anyfs-native-smoke.json';
            const fs = await import('node:fs/promises');
            await fs.writeFile(out, list);
            console.log(`[native:smoke] wrote disk list for ${img} to ${out}`);
            m.sessionClose(h);
        } catch (e) {
            console.error('[native:smoke] failed:', e);
            app.exit(1);
            return;
        }
        app.exit(0);
        return;
    }

    if (!isDev) protocol.handle('anyfs', handleAnyfsRequest);
    // The url proxy must be live in dev too — the renderer's worker still
    // rewrites URLs through it when running against the vite dev server.
    protocol.handle('anyfs-url', handleAnyfsUrlRequest);
    installDownloadIpc();
    installDialogIpc();
    installDrivesIpc();
    installAnyfsNativeIpc();
    createWindow();

    app.on('activate', () => {
        // macOS: re-open a window when the dock icon is clicked.
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
});
