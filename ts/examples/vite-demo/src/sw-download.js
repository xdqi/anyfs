// Streaming download Service Worker.
//
// Lets us trigger a *native* browser download (no Save As dialog, progress
// shown by the browser's download manager) for data we produce on the JS
// side. The technique is the StreamSaver.js trick:
//
//   1. Client registers an entry: a UUID, a filename, and a MessagePort that
//      the SW will pull bytes from.
//   2. Client navigates a hidden iframe to /__streamsave/<uuid>.
//   3. The SW intercepts that request with fetch event, looks up the entry,
//      and returns a Response whose body is a ReadableStream fed by the
//      MessagePort, plus a Content-Disposition: attachment header.
//   4. The browser sees attachment headers and routes the response to the
//      download manager, streaming straight to disk.
//
// No SharedArrayBuffer reachable here — the MessageChannel transfers fresh
// ArrayBuffer copies the client makes before each postMessage.

const PREFIX = '/__streamsave/';
const pending = new Map(); // url -> { port, fileName, size, queue, controller, done, abortReason }

self.addEventListener('install', (event) => {
    event.waitUntil(self.skipWaiting());
});

self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('message', (event) => {
    const data = event.data;
    if (!data || typeof data !== 'object') return;
    if (data.kind === 'streamsave:register') {
        const { url, fileName, size, port } = data;
        const entry = {
            port,
            fileName,
            size: typeof size === 'number' && size >= 0 ? size : null,
            queue: [],
            controller: null,
            done: false,
            abortReason: null,
        };
        port.onmessage = (ev) => onPortMessage(entry, ev.data);
        pending.set(url, entry);
        // Tell the client the entry is live BEFORE they trigger the
        // iframe navigation. Without this, Firefox can dispatch the
        // fetch event to this SW before processing this register
        // message; pending.get() then returns undefined, we don't
        // respondWith, and the iframe navigation falls through to
        // the network. (Chrome happens to order message-before-fetch
        // consistently for this pattern; Firefox doesn't.)
        try { port.postMessage({ kind: 'ready' }); } catch {}
    }
});

function onPortMessage(entry, msg) {
    if (!msg) return;
    if (msg.kind === 'chunk' && msg.data) {
        const chunk = msg.data instanceof Uint8Array ? msg.data : new Uint8Array(msg.data);
        if (entry.controller) entry.controller.enqueue(chunk);
        else entry.queue.push(chunk);
        return;
    }
    if (msg.kind === 'end') {
        entry.done = true;
        if (entry.controller) entry.controller.close();
        return;
    }
    if (msg.kind === 'abort') {
        entry.done = true;
        entry.abortReason = msg.reason ?? 'aborted';
        if (entry.controller) entry.controller.error(new Error(entry.abortReason));
        return;
    }
}

self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);
    if (url.origin !== self.location.origin) return;
    if (!url.pathname.startsWith(PREFIX)) return;
    const entry = pending.get(url.pathname);
    if (!entry) return; // let the network 404 — we don't claim it

    pending.delete(url.pathname);

    const body = new ReadableStream({
        start(controller) {
            entry.controller = controller;
            for (const c of entry.queue) controller.enqueue(c);
            entry.queue = [];
            if (entry.abortReason) controller.error(new Error(entry.abortReason));
            else if (entry.done) controller.close();
        },
        cancel(reason) {
            try { entry.port.postMessage({ kind: 'cancel', reason: String(reason ?? '') }); } catch {}
            try { entry.port.close(); } catch {}
        },
    });

    const headers = new Headers({
        'Content-Type': 'application/octet-stream',
        'Content-Disposition': `attachment; filename*=UTF-8''${encodeURIComponent(entry.fileName)}`,
        // Disable any caching on the way through.
        'Cache-Control': 'no-store',
        // Required so the browser routes through the download manager rather
        // than trying to inline.
        'X-Content-Type-Options': 'nosniff',
    });
    if (entry.size != null) headers.set('Content-Length', String(entry.size));

    event.respondWith(new Response(body, { headers }));
});
