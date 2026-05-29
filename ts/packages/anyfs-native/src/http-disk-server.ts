/*
 * HTTP disk proxy — one server per disk image.
 *
 * Each instance listens on 127.0.0.1 with a random port and proxies
 * Range requests to a single upstream URL (or later, a physical disk).
 * QEMU's curl block driver connects via plain HTTP — no TLS, no OpenSSL,
 * no aio-win32 event loop inside QEMU.
 *
 *     Remote HTTPS ──→ Node.js fetch() (+ Range, follows redirects)
 *                          ↓ http://127.0.0.1:<random-port>/
 *                  QEMU curl driver (plain HTTP, no TLS)
 *
 * The per-disk port model means closing a disk shuts down its server.
 * Later, a privileged process can expose a physical drive the same way:
 * just run a standalone HTTP server on a well-known port that translates
 * Range → pread() on \\.\PhysicalDriveN.
 */

import { createServer } from 'node:http';
import { Readable } from 'node:stream';
import type { Server, IncomingMessage, ServerResponse } from 'node:http';

export class HttpDiskServer {
    private server: Server | null = null;
    private upstreamUrl: string | null = null;
    private contentLength = 0;
    private acceptRanges = true;
    port = 0;

    /** Start listening and return the proxy URL for this disk. */
    async start(upstreamUrl: string): Promise<string> {
        if (this.server) throw new Error('HttpDiskServer: already started');

        // Probe upstream with HEAD (fetch follows redirects by default).
        const info = await this._head(upstreamUrl);
        this.upstreamUrl = upstreamUrl;
        this.contentLength = info.contentLength;
        this.acceptRanges = info.acceptRanges;

        this.server = createServer((req, res) => this._handle(req, res));
        await new Promise<void>((resolve) => this.server!.listen(0, '127.0.0.1', resolve));
        this.port = (this.server!.address() as { port: number }).port;
        return `http://127.0.0.1:${this.port}/`;
    }

    async stop(): Promise<void> {
        if (!this.server) return;
        await new Promise<void>((resolve) => this.server!.close(() => resolve()));
        this.server = null;
        this.port = 0;
        this.upstreamUrl = null;
    }

    // ── internals ──────────────────────────────────────────────────────────

    private async _head(url: string): Promise<{ contentLength: number; acceptRanges: boolean }> {
        const resp = await fetch(url, {
            method: 'HEAD',
            redirect: 'follow',
            signal: AbortSignal.timeout(15000),
        });
        const cl = parseInt(resp.headers.get('content-length') ?? '0', 10);
        const ar = resp.headers.get('accept-ranges') === 'bytes';
        return { contentLength: cl, acceptRanges: ar };
    }

    private _handle(req: IncomingMessage, res: ServerResponse): void {
        if (req.method === 'HEAD') {
            res.writeHead(200, {
                'Content-Length': String(this.contentLength),
                'Accept-Ranges': this.acceptRanges ? 'bytes' : 'none',
                Connection: 'keep-alive',
            });
            res.end();
            return;
        }

        if (req.method === 'GET') {
            this._proxyGet(req, res);
            return;
        }

        res.writeHead(405, { 'Content-Type': 'text/plain' });
        res.end('method not allowed');
    }

    private async _proxyGet(req: IncomingMessage, res: ServerResponse): Promise<void> {
        const headers: Record<string, string> = {};
        const range = req.headers['range'];
        if (range) headers['Range'] = range;

        let upstreamResp: Response;
        try {
            upstreamResp = await fetch(this.upstreamUrl!, {
                method: 'GET',
                headers,
                redirect: 'follow',
                signal: AbortSignal.timeout(60000),
            });
        } catch (err) {
            if (!res.headersSent) {
                res.writeHead(502, { 'Content-Type': 'text/plain' });
                res.end(`upstream fetch error: ${(err as Error).message}`);
            }
            return;
        }

        // Forward status and headers from upstream
        const respHeaders: Record<string, string> = {};
        upstreamResp.headers.forEach((v, k) => {
            respHeaders[k] = v;
        });
        res.writeHead(upstreamResp.status, respHeaders);

        // Stream the body, with abort forwarding from client
        if (upstreamResp.body) {
            const nodeStream = Readable.fromWeb(upstreamResp.body as any);
            res.on('close', () => {
                nodeStream.destroy();
            });
            nodeStream.pipe(res);
        } else {
            res.end();
        }
    }
}
