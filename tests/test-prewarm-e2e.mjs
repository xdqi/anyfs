#!/usr/bin/env node
// End-to-end test: vite-demo main page with prewarm
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import http from 'node:http';
import { CDPClient } from './diagnostics/common-cdp.mjs';

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

const viteDir = resolve(__dirname, '../ts/examples/vite-demo');
const viteProc = spawn('npx', ['vite', '--port', '5209'], {
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

const xvfb = spawn('Xvfb', [':95', '-screen', '0', '1280x720x24'], { stdio: 'ignore' });
await sleep(500);

const cdpPort = 9409;
const electronDir = resolve(__dirname, '../ts/examples/electron-demo');
const electronBin = resolve(electronDir, 'node_modules/.bin/electron');
const electronProc = spawn(electronBin, [
  electronDir, `--remote-debugging-port=${cdpPort}`
], {
  cwd: electronDir,
  stdio: ['ignore', 'pipe', 'pipe'],
  env: { ...process.env, DISPLAY: ':95', ELECTRON_DEV: '1', ANYFS_DISABLE_NATIVE: '1' }
});
electronProc.stderr.on('data', c => process.stderr.write(c));

await sleep(8000);

let targets;
try {
  targets = JSON.parse(await httpGet(`http://127.0.0.1:${cdpPort}/json`));
  console.log('targets:', targets.map(t => `${t.type}:${t.title}`).join(', '));
} catch(e) {
  console.log('Failed to get targets:', e.message);
  electronProc.kill(); xvfb.kill(); viteProc.kill();
  process.exit(1);
}

const page = targets.find(t => t.type === 'page' && !t.url.startsWith('devtools://'));
if (!page) { console.log('NO PAGE'); process.exit(1); }
console.log('page:', page.title, page.url);

const client = new CDPClient(page.webSocketDebuggerUrl);
await client.connect();
await client.send('Runtime.enable');
await client.send('Page.enable');

// Navigate to main page (with prewarm enabled)
console.log('navigating to main page...');
await client.send('Page.navigate', { url: viteUrl + '/' });
await sleep(2000);

// Drain startup console
for (const e of client.events) {
  if (e.method === 'Runtime.consoleAPICalled') {
    const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
    console.log(`  [startup] ${text.substring(0, 300)}`);
  }
}
client.events.length = 0;

// Poll for prewarm completion
let hasResult = false;
for (let i = 0; i < 30; i++) {
  await sleep(1000);

  for (const e of client.events) {
    if (e.method === 'Runtime.consoleAPICalled') {
      const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
      console.log(`  [console] ${text.substring(0, 400)}`);
      if (text.includes('[PREWARM] boot complete')) {
        console.log('\n>>> PREWARM SUCCESS');
        hasResult = true;
      }
      if (text.includes('PREWARM') && text.includes('error')) {
        console.log('\n>>> PREWARM ERROR');
        hasResult = true;
      }
    }
  }
  client.events.length = 0;

  if (hasResult) break;

  // Also check page status text
  try {
    const status = await Promise.race([
      client.evaluate('document.body?.textContent?.substring(0, 200) || "no-body"'),
      new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 3000))
    ]);
    console.log(`  [${i+1}s] body: ${(status || '').replace(/\n/g, ' ').substring(0, 150)}`);
  } catch(e) {
    console.log(`  [${i+1}s] eval timeout`);
  }
}

if (!hasResult) console.log('\n>>> NO RESULT after 30s');

electronProc.kill();
xvfb.kill();
viteProc.kill();
console.log('done');
