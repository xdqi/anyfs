import { createReadStream, statSync } from 'node:fs';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';

export interface RangeServer {
    url: string; // http://127.0.0.1:<port>/image
    close(): Promise<void>;
}

/** Serve a single file over HTTP with Range support (URLFS needs partial reads
 *  and a HEAD that advertises Accept-Ranges + Content-Length). */
export async function serveFileWithRange(file: string): Promise<RangeServer> {
    const size = statSync(file).size;
    const server: Server = createServer((req, res) => {
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
