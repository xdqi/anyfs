// Smoke: open a local image over http:// to validate QEMU's curl block driver
// is linked correctly into anyfs_native.node.
//
// The HTTP server runs in a Worker thread because anyfs_native's sessionOpen is
// synchronous: it blocks the main Node thread inside QEMU coroutines, which
// then issue libcurl requests we must serve concurrently. A same-thread http
// server would deadlock.
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';

if (!isMainThread) {
    const http = await import('node:http');
    const { createReadStream, statSync, openSync, readSync, closeSync } = await import('node:fs');
    const img = workerData.img;
    const size = statSync(img).size;
    const server = http.createServer((req, res) => {
        const range = req.headers['range'];
        if (req.method === 'HEAD') {
            res.writeHead(200, {
                'Content-Length': String(size),
                'Accept-Ranges': 'bytes',
                'Content-Type': 'application/octet-stream',
            });
            res.end();
            return;
        }
        if (range) {
            const m = /^bytes=(\d+)-(\d+)?$/.exec(range);
            if (!m) {
                res.writeHead(416);
                res.end();
                return;
            }
            const start = Number(m[1]);
            const end = m[2] ? Number(m[2]) : size - 1;
            const len = end - start + 1;
            res.writeHead(206, {
                'Content-Range': `bytes ${start}-${end}/${size}`,
                'Accept-Ranges': 'bytes',
                'Content-Length': String(len),
                'Content-Type': 'application/octet-stream',
            });
            const fd = openSync(img, 'r');
            const buf = Buffer.alloc(len);
            readSync(fd, buf, 0, len, start);
            closeSync(fd);
            res.end(buf);
            return;
        }
        res.writeHead(200, {
            'Content-Length': String(size),
            'Accept-Ranges': 'bytes',
            'Content-Type': 'application/octet-stream',
        });
        createReadStream(img).pipe(res);
    });
    server.listen(0, '127.0.0.1', () => {
        parentPort.postMessage({ port: server.address().port });
    });
} else {
    const require = createRequire(import.meta.url);
    const here = dirname(fileURLToPath(import.meta.url));
    const n = require('../index.js');
    const img = resolve(here, '../../../examples/vite-demo/public/disks/multi.img');

    const worker = new Worker(new URL(import.meta.url), { workerData: { img } });
    const port = await new Promise((res) => worker.once('message', (m) => res(m.port)));
    const url = `http://127.0.0.1:${port}/multi.img`;
    console.log('[smoke-url] serving', img, 'at', url);

    try {
        console.log('[smoke-url] kernelInit(64, 0)');
        if (n.kernelInit(64, 0) !== 0) process.exit(3);

        // ANYFS_SESSION_READONLY = 1 — required for the QEMU curl driver which is
        // read-only by design.
        console.log('[smoke-url] sessionOpen', url, '(RDONLY)');
        const h = n.sessionOpen(url, 1);
        if (h < 0) {
            console.error('open rc=', h);
            process.exit(4);
        }

        const meta = JSON.parse(n.sessionMetaJson(h));
        console.log('[smoke-url] sessionMeta:', meta);

        const parts = JSON.parse(n.sessionListJson(h));
        console.log(
            '[smoke-url] partitions:',
            parts.length,
            parts.map((p) => `${p.fstype}/${p.label}`).join(', '),
        );
        if (parts.length === 0) process.exit(5);

        const pick = parts.find((p) => p.fstype === 'ext2') ?? parts[0];
        console.log(
            `[smoke-url] sessionEnter(part=${pick.index} ${pick.fstype}/${pick.label}, RDONLY)`,
        );
        const mount = n.sessionEnter(h, pick.index, 1);
        console.log('  mounted at', mount);

        const entries = JSON.parse(n.readdirJson(mount));
        console.log('[smoke-url] readdir:', entries.length, 'entries');
        console.log(' ', entries.slice(0, 5));

        if (n.sessionClose(h) !== 0) console.warn('sessionClose nonzero');
        console.log('[smoke-url] OK');
    } finally {
        await worker.terminate();
    }
}
