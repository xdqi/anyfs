/*
 * common.mjs — Shared utilities for CDP-based UI tests.
 *
 * - Launchers for Electron, Chromium, vite dev server, HTTP file server
 * - CDP interaction helpers (wait for kernel, trigger file open, poll for tree)
 */
import { spawn, execSync } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync, statSync, openSync, readSync } from 'node:fs';
import { createServer } from 'node:http';
import { networkInterfaces } from 'node:os';
import { CDPClient, sleep } from './common-cdp.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..');
const TS = resolve(ROOT, 'ts');
const ELECTRON_DIR = resolve(TS, 'examples', 'electron-demo');

export const GREEN = '\x1b[32m';
export const RED = '\x1b[31m';
export const BOLD = '\x1b[1m';
export const RST = '\x1b[0m';

// ── Chrome ───────────────────────────────────────────────────────────────

export function findChrome() {
  for (const bin of ['google-chrome', 'chromium', 'chromium-browser']) {
    try { return execSync(`which ${bin}`, { stdio: 'pipe' }).toString().trim(); } catch {}
  }
  return null;
}

export function startChromium(port, url) {
  const bin = findChrome();
  if (!bin) throw new Error('No Chrome/Chromium found');
  const dir = `/tmp/chrome-cdp-test-${Date.now()}`;
  const proc = spawn(bin, [
    `--remote-debugging-port=${port}`, `--user-data-dir=${dir}`,
    '--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
    url,
  ], { stdio: 'ignore', detached: false });
  return proc;
}

// ── vite dev server ──────────────────────────────────────────────────────

export function startVite() {
  return new Promise((resolve, reject) => {
    const proc = spawn('pnpm', ['--filter', 'vite-demo', 'dev'],
      { cwd: TS, stdio: ['ignore', 'pipe', 'pipe'] });
    let url = null;
    const timer = setTimeout(() => {
      if (!url) reject(new Error('vite dev server timeout'));
    }, 30000);
    proc.stdout.on('data', c => {
      const s = c.toString();
      const m = s.match(/Local:\s+(http:\/\/[^\s]+)/);
      if (m) { url = m[1]; clearTimeout(timer); resolve({ proc, url }); }
    });
    proc.stderr.on('data', () => {});
  });
}

// ── Electron ─────────────────────────────────────────────────────────────

export function startElectron(port, extraEnv = {}) {
  // Use a random display number to avoid conflicts with stale Xvfb instances
  const displayNum = 90 + Math.floor(Math.random() * 10);
  const display = `:${displayNum}`;
  const xvfb = spawn('Xvfb', [display, '-screen', '0', '1280x1024x24'],
    { stdio: 'ignore' });

  const electronBin = resolve(ELECTRON_DIR, 'node_modules', '.bin', 'electron');
  const proc = spawn('env', [
    '-u', 'ELECTRON_RUN_AS_NODE',
    `DISPLAY=${display}`,
    ...Object.entries(extraEnv).map(([k, v]) => `${k}=${v}`),
    electronBin, ELECTRON_DIR, `--remote-debugging-port=${port}`,
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let stderr = '';
  proc.stderr.on('data', c => { stderr += c.toString(); process.stderr.write(c); });
  proc.stdout.on('data', c => { process.stdout.write(c); });
  return { proc, xvfb, getStderr: () => stderr };
}

// ── HTTP file server (Range-aware) ───────────────────────────────────────

export function startHttpServer(filePath) {
  const stat = statSync(filePath);
  const size = stat.size;
  let fd = null;

  const server = createServer((req, res) => {
    if (fd === null) fd = openSync(filePath, 'r');
    const corsHeaders = {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, HEAD, OPTIONS',
      'Access-Control-Allow-Headers': 'Range',
      'Access-Control-Expose-Headers': 'Content-Length, Content-Range, Accept-Ranges',
    };
    if (req.method === 'OPTIONS') {
      res.writeHead(204, corsHeaders);
      res.end();
      return;
    }
    if (req.method === 'HEAD') {
      res.writeHead(200, {
        'Content-Length': String(size),
        'Accept-Ranges': 'bytes',
        'Content-Type': 'application/octet-stream',
        ...corsHeaders,
      });
      res.end();
      return;
    }
    let [start, end] = [0, size - 1];
    let status = 200;
    const r = req.headers['range'];
    if (r) {
      const m = r.match(/bytes=(\d+)-(\d*)/);
      if (m) { start = parseInt(m[1], 10); end = m[2] ? parseInt(m[2], 10) : size - 1; status = 206; }
    }
    const length = end - start + 1;
    res.writeHead(status, {
      'Content-Length': String(length),
      'Content-Range': `bytes ${start}-${end}/${size}`,
      'Accept-Ranges': 'bytes',
      'Content-Type': 'application/octet-stream',
      ...corsHeaders,
    });
    const buf = Buffer.alloc(65536);
    let pos = start, remaining = length;
    const pump = () => {
      if (remaining <= 0) { res.end(); return; }
      const chunk = Math.min(buf.length, remaining);
      try {
        const n = readSync(fd, buf, 0, chunk, pos);
        if (n <= 0) { res.end(); return; }
        res.write(buf.subarray(0, n));
        pos += n; remaining -= n;
        if (remaining > 0 && n > 0) setImmediate(pump);
        else res.end();
      } catch { res.destroy(); }
    };
    pump();
  });

  return new Promise((resolve, reject) => {
    server.listen(0, '127.0.0.1', () => {
      resolve({ server, port: server.address().port, size });
    });
    server.on('error', reject);
  });
}

// ── CDP interaction helpers ──────────────────────────────────────────────

/**
 * Connect CDP to a browser target at host:port, returning the first page client.
 */
export async function connectCDP(host, port) {
  const targets = await CDPClient.listTargets(host, port);
  // Find the anyfs app page (not DevTools, not service workers)
  const page = targets.find(t =>
    t.type === 'page' && t.title.toLowerCase().includes('anyfs')
  ) || targets.find(t => t.type === 'page');
  if (!page) throw new Error('No page target found');
  const client = new CDPClient(page.webSocketDebuggerUrl);
  await client.connect();
  await client.send('Runtime.enable');
  await client.send('Page.enable');
  return { client, page };
}

/**
 * Wait for the LKL kernel to finish booting. Returns true if boot detected.
 */
export async function waitForKernel(client, timeoutMs = 30000) {
  try {
    await client.waitForConsole('Linux version', timeoutMs);
    return 'boot-log';
  } catch {
    const coi = await client.evaluate('crossOriginIsolated');
    return coi ? 'coi-ready' : false;
  }
}

/**
 * Click "Open URL…" to bring up the URL modal, type a URL, and click Open.
 * Clicks the dialog's "Open" button rather than pressing Enter, because
 * React state batching can cause the URL value to be stale in keydown handlers.
 */
export async function typeUrl(client, url) {
  // Click the "Open URL…" button on the landing page
  const btn = await client.evaluate(`
    (() => {
      const btns = Array.from(document.querySelectorAll('button'));
      const b = btns.find(x => x.textContent.includes('Open URL'));
      if (!b) return 'no-open-url-btn';
      b.click();
      return 'clicked';
    })()
  `);
  if (btn !== 'clicked') return `no-open-url-btn: ${btn}`;

  await sleep(500);

  // Fill the URL input
  const fill = await client.evaluate(`
    (() => {
      const input = document.querySelector('input[aria-label="Disk image URL"]');
      if (!input) return 'no-input';
      const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
      setter.call(input, ${JSON.stringify(url)});
      input.dispatchEvent(new Event('input', { bubbles: true }));
      return 'ok';
    })()
  `);
  if (fill !== 'ok') return fill;

  await sleep(200);

  // Click the "Open" button in the dialog (not Enter on the input — that
  // races with React's batched state update for the URL value).
  return client.evaluate(`
    (() => {
      const dialog = document.querySelector('[role="dialog"]');
      if (!dialog) return 'no-dialog';
      for (const b of dialog.querySelectorAll('button')) {
        if (b.textContent.trim() === 'Open') {
          if (b.disabled) return 'btn-disabled';
          b.click();
          return 'ok';
        }
      }
      return 'no-open-btn';
    })()
  `);
}

/**
 * Click the "Open file…" button (native Electron path).
 * Requires ANYFS_TEST_LOCAL_PATH to be set in the Electron env so the
 * native OS file dialog is skipped (CDP can't automate OS dialogs).
 */
export async function clickOpenImage(client) {
  const result = await client.evaluate(`
    (() => {
      const btns = Array.from(document.querySelectorAll('button'));
      const openBtn = btns.find(b =>
        b.textContent.includes('Open file') ||
        b.textContent.includes('Open')
      );
      if (!openBtn) return 'no-btn: ' + btns.slice(0,5).map(b=>b.textContent.trim()).join(', ');
      openBtn.click();
      return 'clicked';
    })()
  `);
  return result;
}

/**
 * Click the first partition entry in the partition list.
 * Returns the partition text that was clicked, or error description.
 */
export async function clickFirstPartition(client) {
  return client.evaluate(`
    (() => {
      // Find partition buttons — they contain '#1', '#2', etc.
      const btns = Array.from(document.querySelectorAll('button'));
      const partBtn = btns.find(b => /^#\\d+/.test(b.textContent.trim()));
      if (!partBtn) return 'no-partition-btn';
      const label = partBtn.textContent.trim().substring(0, 60);
      partBtn.click();
      return 'clicked: ' + label;
    })()
  `);
}

/**
 * Poll the DOM for evidence that a disk has been mounted and files are visible.
 * Returns the number of file entries found, or 0 on timeout.
 */
export async function waitForFileTree(client, timeoutMs = 120000) {
  const deadline = Date.now() + timeoutMs;
  let iter = 0;
  while (Date.now() < deadline) {
    iter++;
    const result = await client.evaluate(`
      (() => {
        // Chonky file browser: class-based selectors
        const chonkyRows = document.querySelectorAll('[class*="chonky"] [role="button"]');
        if (chonkyRows.length > 0) return chonkyRows.length;
        // Role-based tree
        const treeItems = document.querySelectorAll('[role="tree"] [role="treeitem"]');
        if (treeItems.length > 0) return treeItems.length;
        // Generic file-listing classes
        const ptItems = document.querySelectorAll('.file-list-item, [class*="FileList"], [class*="file-list"]');
        if (ptItems.length > 0) return ptItems.length;
        // Any element with a file-path-like text child (Chonky rows)
        const allElems = document.querySelectorAll('[role="button"], [data-rfd-draggable-id], li[aria-label], a[href*="#/"]');
        if (allElems.length > 3) return allElems.length;
        // Text fallback
        const body = document.body?.innerText || '';
        if (/\\b(README|debian|autorun|install|hello|\\.txt|\\.conf|\\.so|bin|etc|lib|usr|boot|home|root|sbin|dev|proc|sys)\\b/i.test(body)) {
          return -1;
        }
        return 0;
      })()
    `);
    if (result !== 0 && result !== undefined) {
      if (result === -1) return 'text-matched';
      return result;
    }
    // Periodic diagnostic — log body + recent console messages
    if (iter % 5 === 0) {
      const body = await client.evaluate('document.body?.innerText?.substring(0, 200) || "(empty)"');
      console.log(`  [poll #${iter}] body: ${body.replace(/\\n/g, ' ').slice(0, 150)}`);
      // Show all recent console messages for diagnostics
      const msgs = client.events.filter(e =>
        e.method === 'Runtime.consoleAPICalled'
      ).slice(-20);
      for (const e of msgs) {
        const txt = (e.params?.args || []).map(a => a.value || a.description || '').join(' ');
        console.log(`  [console ${e.params?.type}] ${txt.slice(0, 300)}`);
      }
    }
    await sleep(2000);
  }
  return 0;
}

/**
 * Find a disk image, respecting --image= arg or falling back to defaults.
 */
export function findImage() {
  const arg = process.argv.find(a => a.startsWith('--image='))?.split('=')[1];
  if (arg) {
    if (!existsSync(arg)) throw new Error(`image not found: ${arg}`);
    return arg;
  }
  const candidates = [
    resolve(ROOT, 'tests', 'images', 'ext4.img'),
  ];
  for (const c of candidates) if (existsSync(c)) return c;
  throw new Error('No disk image found. Pass --image=<path>.');
}

/**
 * Check for JS errors collected by the app's error handler.
 */
export async function checkJsErrors(client) {
  const count = await client.evaluate('(window.__jsErrors || []).length');
  if (count > 0) {
    const errors = await client.evaluate(
      'JSON.stringify((window.__jsErrors || []).slice(0, 5))');
    return { count, sample: errors };
  }
  return { count: 0, sample: null };
}
