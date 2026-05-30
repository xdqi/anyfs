import { createReadStream, statSync } from 'node:fs';
import { createServer, type Server, type ServerResponse } from 'node:http';
import type { AddressInfo } from 'node:net';

export interface RangeServer {
    url: string; // http://127.0.0.1:<port>/image
    close(): Promise<void>;
}

/** The page origin (http://localhost:4199 on web, anyfs:// on electron) differs
 *  from this 127.0.0.1:<port> server, so URLFS's in-worker fetch/XHR is a
 *  cross-origin request: without CORS the browser blocks it before any byte is
 *  read. Allow any origin and expose the Range-reply headers URLFS needs to see
 *  (Content-Range / Accept-Ranges / Content-Length). The real remote source
 *  (the @network variant) is reachable cross-origin in its own right; this only
 *  re-creates that for the hermetic local server. */
function cors(res: ServerResponse): void {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Range');
    res.setHeader('Access-Control-Expose-Headers', 'Content-Range, Accept-Ranges, Content-Length');
}

/** Serve a single file over HTTP with Range support (URLFS needs partial reads
 *  and a HEAD that advertises Accept-Ranges + Content-Length). */
export async function serveFileWithRange(file: string): Promise<RangeServer> {
    const size = statSync(file).size;
    const server: Server = createServer((req, res) => {
        cors(res);
        if (req.method === 'OPTIONS') {
            res.writeHead(204);
            return res.end();
        }
        if (req.method === 'HEAD') {
            res.writeHead(200, {
                'Accept-Ranges': 'bytes',
                'Content-Length': String(size),
            });
            return res.end();
        }
        const range = req.headers.range;
        const m = range ? /bytes=(\d+)-(\d*)/.exec(range) : null;
        if (m) {
            const start = Number(m[1]);
            const end = m[2] ? Number(m[2]) : size - 1;
            res.writeHead(206, {
                'Accept-Ranges': 'bytes',
                'Content-Range': `bytes ${start}-${end}/${size}`,
                'Content-Length': String(end - start + 1),
            });
            return createReadStream(file, { start, end }).pipe(res);
        }
        res.writeHead(200, { 'Accept-Ranges': 'bytes', 'Content-Length': String(size) });
        createReadStream(file).pipe(res);
    });
    await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
    const port = (server.address() as AddressInfo).port;
    return {
        url: `http://127.0.0.1:${port}/image`,
        close: () => new Promise<void>((r) => server.close(() => r())),
    };
}

/** Serve a single file over HTTP that DELIBERATELY does not support Range:
 *  no Accept-Ranges header, Range requests are ignored and always answered
 *  with a full 200 body. Drives the app's "URL without Range support" error
 *  path. */
export async function serveFileNoRange(file: string): Promise<RangeServer> {
    const size = statSync(file).size;
    const server: Server = createServer((req, res) => {
        cors(res);
        if (req.method === 'OPTIONS') {
            res.writeHead(204);
            return res.end();
        }
        if (req.method === 'HEAD') {
            res.writeHead(200, { 'Content-Length': String(size) });
            return res.end();
        }
        res.writeHead(200, { 'Content-Length': String(size) });
        createReadStream(file).pipe(res);
    });
    await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
    const port = (server.address() as AddressInfo).port;
    return {
        url: `http://127.0.0.1:${port}/image`,
        close: () => new Promise<void>((r) => server.close(() => r())),
    };
}
