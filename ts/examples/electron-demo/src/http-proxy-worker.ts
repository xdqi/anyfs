/*
 * Worker-thread HTTP proxy for remote disk images.
 *
 * Each Worker runs a tiny HTTP server on 127.0.0.1:<random> that proxies
 * Range requests to a single upstream URL via Node's fetch() (which
 * follows redirects and handles TLS natively).
 *
 * QEMU's curl driver connects via plain HTTP to this server. Because the
 * addon calls are synchronous and block the calling thread's event loop,
 * the server MUST live in a separate thread with its own libuv loop.
 *
 * parentPort protocol:
 *   worker receives { upstreamUrl } via workerData
 *   worker sends    { port }          once listening
 *   worker receives 'stop'            to shut down
 */

import { parentPort, workerData } from 'node:worker_threads';
import { createServer } from 'node:http';
import { Readable } from 'node:stream';
import { statSync, createReadStream } from 'node:fs';
import type { IncomingMessage, ServerResponse } from 'node:http';

const { upstreamUrl, localPath } = workerData as {
    upstreamUrl?: string;
    localPath?: string;
};

let contentLength = 0;
let acceptRanges = true;

function log(msg: string) {
    const ts = new Date().toISOString();
    console.error(`[http-proxy-worker ${ts}] ${msg}`);
}

async function main() {
    const label = upstreamUrl ?? localPath ?? '?';
    log(`starting for ${label}`);

    if (localPath) {
        // Local file mode: stat for size, serve with Range support.
        try {
            const st = statSync(localPath);
            contentLength = st.size;
            acceptRanges = true;
        } catch (err) {
            log(`stat failed: ${(err as Error).message}`);
            parentPort!.postMessage({ error: `stat failed: ${(err as Error).message}` });
            process.exit(1);
            return;
        }
        log(`localPath OK: size=${contentLength}`);
    } else {
        // Remote URL mode: HEAD probe via fetch.
        log(`typeof fetch=${typeof fetch} globalThis.fetch=${typeof (globalThis as any).fetch}`);
        let headResp: Response;
        try {
            headResp = await fetch(upstreamUrl!, {
                method: 'HEAD',
                redirect: 'follow',
                signal: AbortSignal.timeout(15000),
            });
        } catch (err) {
            const e = err as Error;
            const cause = (e as any).cause;
            log(
                `HEAD probe failed: message="${e.message}" cause="${cause?.message}" code="${cause?.code}" name="${e.name}" stack="${e.stack?.substring(0, 300)}"`,
            );
            parentPort!.postMessage({
                error: `HEAD probe failed: ${e.message} (cause: ${cause?.message || 'none'})`,
            });
            process.exit(1);
            return;
        }
        contentLength = parseInt(headResp.headers.get('content-length') ?? '0', 10);
        acceptRanges = headResp.headers.get('accept-ranges') === 'bytes';
        log(`HEAD probe OK: content-length=${contentLength}, accept-ranges=${acceptRanges}`);
    }

    // ── HTTP server ──────────────────────────────────────────────────────
    const server = createServer((req, res) => {
        log(`${req.method} ${req.url} headers=${JSON.stringify(req.headers)}`);
        if (req.method === 'HEAD') {
            res.writeHead(200, {
                'Content-Length': String(contentLength),
                'Accept-Ranges': acceptRanges ? 'bytes' : 'none',
                Connection: 'keep-alive',
            });
            res.end();
            return;
        }
        if (req.method === 'GET') {
            if (localPath) {
                serveLocalFile(req, res);
                return;
            }
            proxyGet(req, res);
            return;
        }
        res.writeHead(405, { 'Content-Type': 'text/plain' });
        res.end('method not allowed');
    });

    server.on('error', (err) => {
        log(`server error: ${err.message}`);
    });

    server.listen(0, '127.0.0.1', () => {
        const port = (server.address() as { port: number }).port;
        log(`listening on 127.0.0.1:${port}`);
        parentPort!.postMessage({ port });
    });

    parentPort!.on('message', (msg) => {
        if (msg === 'stop') {
            log('received stop, closing server');
            server.close();
        }
    });
}

function serveLocalFile(req: IncomingMessage, res: ServerResponse) {
    const total = contentLength;
    const { range } = req.headers as { range?: string };

    if (range) {
        const m = /^bytes=(\d+)-(\d*)$/.exec(range);
        if (m) {
            const start = parseInt(m[1], 10);
            const end = m[2] ? parseInt(m[2], 10) : total - 1;
            if (start >= total) {
                res.writeHead(416, { 'Content-Range': `bytes */${total}` });
                res.end();
                return;
            }
            res.writeHead(206, {
                'Content-Range': `bytes ${start}-${end}/${total}`,
                'Content-Length': String(end - start + 1),
                'Accept-Ranges': 'bytes',
            });
            createReadStream(localPath!, { start, end }).pipe(res);
            return;
        }
    }
    res.writeHead(200, {
        'Content-Length': String(total),
        'Accept-Ranges': 'bytes',
    });
    createReadStream(localPath!).pipe(res);
}

async function proxyGet(req: IncomingMessage, res: ServerResponse) {
    const range = req.headers['range'];
    const headers: Record<string, string> = {};
    if (range) headers['Range'] = range;

    let upResp: Response;
    try {
        upResp = await fetch(upstreamUrl!, {
            method: 'GET',
            headers,
            redirect: 'follow',
            signal: AbortSignal.timeout(60000),
        });
    } catch (err) {
        log(`upstream fetch error: ${(err as Error).message}`);
        if (!res.headersSent) {
            res.writeHead(502, { 'Content-Type': 'text/plain' });
            res.end(`upstream fetch error: ${(err as Error).message}`);
        }
        return;
    }

    log(
        `upstream responded: status=${upResp.status}, content-length=${upResp.headers.get('content-length')}`,
    );

    const respHeaders: Record<string, string> = {};
    upResp.headers.forEach((v, k) => {
        respHeaders[k] = v;
    });
    res.writeHead(upResp.status, respHeaders);

    if (upResp.body) {
        const nodeStream = Readable.fromWeb(upResp.body as any);
        let bytesSent = 0;
        nodeStream.on('data', (chunk: Buffer) => {
            bytesSent += chunk.length;
        });
        nodeStream.on('end', () => {
            log(`stream complete: ${bytesSent} bytes sent to client`);
        });
        nodeStream.on('error', (err: Error) => {
            log(`stream error: ${err.message}`);
        });
        res.on('close', () => {
            log(`client closed after ${bytesSent} bytes`);
            nodeStream.destroy();
        });
        nodeStream.pipe(res);
    } else {
        res.end();
    }
}

main();
