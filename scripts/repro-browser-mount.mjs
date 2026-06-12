// Repro for the browser ext4 mount regression (2026-06-12): disk_enter
// rc=-1 after ~400ms in the browser, while the node bundle mounts the same
// image fine. Drives the "Open file…" path via CDP DOM.setFileInputFiles.
//
// Setup (each in its own shell):
//   1. Serve a built vite-demo dist with COOP/COEP on :4173 — either
//      `pnpm --filter vite-demo build` + the dist Caddyfile
//      (add `{ admin off }` if the prod caddy is running), or the
//      anyfs-web-dist CI artifact extracted anywhere:
//        printf '{\n\tadmin off\n}\n' | cat - Caddyfile > Caddyfile.v
//        caddy run --config Caddyfile.v --adapter caddyfile
//   2. chromium --headless=new --remote-debugging-port=9243 --no-sandbox \
//        --disable-gpu --user-data-dir=/tmp/cdp-profile about:blank
//   3. PATH=/usr/sbin:$PATH bash ts/packages/core/test/make-single-image.sh /tmp/single.img
//   4. node scripts/repro-browser-mount.mjs [/path/to/image]
//
// Expected (bug): UI shows "Can't mount partition #0 / disk_enter rc=-1";
// worker console logs `op=enter id=N ERR after ~400ms: disk_enter rc=-1`.
// attach/listParts/meta succeed (size + "no partition table" probed fine).
// Counterfactual: `node ts/packages/core/test/smoke.node.mjs single` mounts
// the same image OK (park the disks/single.img symlink first).

const CDP = 'http://127.0.0.1:9243';
const APP = 'http://127.0.0.1:4173/';
const IMG_PATH = process.argv[2] ?? '/tmp/single.img';
const OUT = '/tmp/repro-browser-mount.png';

const tab = await (await fetch(`${CDP}/json/new?${encodeURIComponent(APP)}`, { method: 'PUT' })).json();
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));

let nid = 0;
const pending = new Map();
const logs = [];
ws.addEventListener('message', (ev) => {
    const msg = JSON.parse(ev.data.toString());
    if (msg.id !== undefined && pending.has(msg.id)) {
        const { resolve, reject } = pending.get(msg.id);
        pending.delete(msg.id);
        msg.error ? reject(new Error(JSON.stringify(msg.error))) : resolve(msg.result);
    } else if (msg.method === 'Runtime.consoleAPICalled') {
        logs.push(msg.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
    }
});
const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++nid; pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
});
const evaluate = async (expression) =>
    (await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })).result.value;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

await send('Page.enable');
await send('Runtime.enable');
await send('DOM.enable');

// Wait for the landing prewarm to finish booting the kernel.
for (let i = 0; i < 60; i++) {
    const s = await evaluate(`document.body.innerText.toLowerCase()`);
    if (s && !s.includes('booting kernel')) break;
    await sleep(1000);
}
console.log('kernel booted');

const { root } = await send('DOM.getDocument');
const { nodeId } = await send('DOM.querySelector', { nodeId: root.nodeId, selector: 'input[type=file]' });
if (!nodeId) { console.error('no file input found'); process.exit(2); }
await send('DOM.setFileInputFiles', { files: [IMG_PATH], nodeId });
console.log('file set:', IMG_PATH);
await sleep(3000);

// Whole-disk image lands on the partition picker — enter entry #0.
for (let i = 0; i < 30; i++) {
    const picked = await evaluate(`(() => {
        const els = [...document.querySelectorAll('a,button,[role=button],div,span,td,tr')];
        const el = els.find(e => /whole disk/i.test(e.textContent || '') && e.textContent.length < 60);
        if (!el) return 'no-picker';
        el.click();
        return 'picked';
    })()`);
    if (picked === 'picked') { console.log('partition: whole-disk clicked'); break; }
    await sleep(1000);
}

let verdict = 'mount-failed';
for (let i = 0; i < 30; i++) {
    const text = await evaluate(`document.body.innerText`);
    if (/hello\.txt/.test(text)) { verdict = 'MOUNTED-OK'; break; }
    if (/can.t mount partition/i.test(text)) { verdict = 'disk_enter-rc-1'; break; }
    await sleep(1000);
}
console.log('verdict:', verdict);
console.log('--- last worker/console lines ---');
console.log(logs.slice(-12).join('\n'));

const { data } = await send('Page.captureScreenshot', { format: 'png' });
await import('node:fs').then((fs) => fs.writeFileSync(OUT, Buffer.from(data, 'base64')));
console.log('shot:', OUT);

await fetch(`${CDP}/json/close/${tab.id}`, { method: 'PUT' });
ws.close();
process.exit(verdict === 'MOUNTED-OK' ? 0 : 1);
