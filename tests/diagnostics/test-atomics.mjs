#!/usr/bin/env node
// Minimal CDP test: launch Electron, navigate to debug-atomics.html, watch log.
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import http from 'node:http';
import { CDPClient } from './common-cdp.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function httpGet(url) {
  return new Promise((res, rej) => {
    http.get(url, (resp) => {
      let d = ''; resp.on('data', c => d += c);
      resp.on('end', () => res(d));
    }).on('error', rej);
  });
}

// ── start vite ───────────────────────────────────────────────────────────
const viteDir = resolve(__dirname, '../../ts/examples/vite-demo');
const viteProc = spawn('npx', ['vite', '--port', '5201'], {
  cwd: viteDir,
  stdio: ['ignore', 'pipe', 'pipe'],
});
const viteUrl = await new Promise((res, rej) => {
  const t = setTimeout(() => rej(new Error('vite timeout')), 30000);
  viteProc.stdout.on('data', (d) => {
    const m = d.toString().match(/Local:\s+(http:\/\/[^\s]+)/);
    if (m) { clearTimeout(t); res(m[1]); }
  });
});
console.log('vite:', viteUrl);

// Verify debug page exists
const debugHtml = await httpGet(viteUrl + '/debug-atomics.html');
console.log('debug page length:', debugHtml.length);

// ── start Xvfb + Electron ────────────────────────────────────────────────
const xvfb = spawn('Xvfb', [':99', '-screen', '0', '1280x720x24'], { stdio: 'ignore' });
await sleep(500);

const cdpPort = 9401;
const electronDir = resolve(__dirname, '../../ts/examples/electron-demo');
const electronBin = resolve(electronDir, 'node_modules/.bin/electron');
const electronProc = spawn(electronBin, [
  electronDir, `--remote-debugging-port=${cdpPort}`
], {
  cwd: electronDir,
  stdio: ['ignore', 'pipe', 'pipe'],
  env: { ...process.env, DISPLAY: ':99', ELECTRON_DEV: '1', ANYFS_DISABLE_NATIVE: '1' }
});
electronProc.stderr.on('data', c => process.stderr.write(c));

await sleep(5000);

// ── connect CDP ──────────────────────────────────────────────────────────
const targets = JSON.parse(await httpGet(`http://127.0.0.1:${cdpPort}/json`));
console.log('targets:', targets.map(t => `${t.type}:${t.title}`).join(', '));

const page = targets.find(t =>
  t.type === 'page' && t.title.toLowerCase().includes('anyfs')
) || targets.find(t => t.type === 'page' && !t.url.startsWith('devtools://'));
if (!page) { console.log('NO PAGE'); process.exit(1); }
console.log('page:', page.title, page.url);

const client = new CDPClient(page.webSocketDebuggerUrl);
await client.connect();
await client.send('Runtime.enable');
await client.send('Page.enable');

// Navigate to debug page
console.log('navigating to debug-atomics.html...');
await client.send('Page.navigate', { url: viteUrl + '/debug-atomics.html' });
await sleep(1000);

// Check current URL
const curUrl = await client.evaluate('window.location.href');
console.log('current URL:', curUrl);

// Monitor console events
for (const e of client.events) {
  if (e.method === 'Runtime.consoleAPICalled') {
    const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
    console.log(`  [console-startup] ${text.substring(0, 200)}`);
  }
}
client.events.length = 0;

// Poll for result
let hasResult = false;
for (let i = 0; i < 15; i++) {
  await sleep(1000);
  const logText = await client.evaluate(
    'document.getElementById("log")?.textContent?.substring(0, 2000) || "no-log"'
  ).catch(() => 'eval-err');

  // Drain console events
  for (const e of client.events) {
    if (e.method === 'Runtime.consoleAPICalled') {
      const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
      console.log(`  [console] ${text.substring(0, 400)}`);
    }
  }
  client.events.length = 0;

  const short = (logText || '').replace(/\n/g, ' | ');
  console.log(`  [${i+1}s] log: ${short}`);

  if (logText?.includes('DONE')) {
    console.log('\n>>> DONE');
    hasResult = true;
    break;
  }
}
if (!hasResult) console.log('\n>>> NO RESULT after 15s');

// ── cleanup ──────────────────────────────────────────────────────────────
electronProc.kill();
xvfb.kill();
viteProc.kill();
console.log('done');
