#!/usr/bin/env node
// Test: load anyfs.mjs directly on main page (not in Worker), call anyfs_ts_init.
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

const viteDir = resolve(__dirname, '../ts/examples/vite-demo');
const viteProc = spawn('npx', ['vite', '--port', '5203'], {
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

const xvfb = spawn('Xvfb', [':97', '-screen', '0', '1280x720x24'], { stdio: 'ignore' });
await sleep(500);

const cdpPort = 9403;
const electronDir = resolve(__dirname, '../ts/examples/electron-demo');
const electronBin = resolve(electronDir, 'node_modules/.bin/electron');

// Use a longer timeout for CDP
const electronProc = spawn(electronBin, [
  electronDir, `--remote-debugging-port=${cdpPort}`
], {
  cwd: electronDir,
  stdio: ['ignore', 'pipe', 'pipe'],
  env: { ...process.env, DISPLAY: ':97', ELECTRON_DEV: '1', ANYFS_DISABLE_NATIVE: '1' }
});
electronProc.stderr.on('data', c => process.stderr.write(c));

// Give Electron more time to start
await sleep(8000);

// Check if we can get the targets
let targets;
try {
  targets = JSON.parse(await httpGet(`http://127.0.0.1:${cdpPort}/json`));
  console.log('targets:', targets.map(t => `${t.type}:${t.title}`).join(', '));
} catch(e) {
  console.log('Failed to get targets:', e.message);
  electronProc.kill();
  xvfb.kill();
  viteProc.kill();
  process.exit(1);
}

const page = targets.find(t =>
  t.type === 'page' && !t.url.startsWith('devtools://')
);
if (!page) { console.log('NO PAGE'); process.exit(1); }
console.log('page:', page.title, page.url);

const client = new CDPClient(page.webSocketDebuggerUrl);
await client.connect();
await client.send('Runtime.enable');
await client.send('Page.enable');
await client.send('Console.enable');

// Navigate to direct-module-test.html
console.log('navigating to direct-module-test.html...');
const navResult = await client.send('Page.navigate', { url: viteUrl + '/direct-module-test.html' });
console.log('nav result:', JSON.stringify(navResult).substring(0, 200));

// Wait and check page state
for (let i = 0; i < 60; i++) {
  await sleep(2000);

  // Try to evaluate - don't fail if it times out
  let logText = 'eval-failed';
  let curUrl = 'url-failed';
  try {
    curUrl = await Promise.race([
      client.evaluate('window.location.href'),
      new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 5000))
    ]);
  } catch(e) {
    curUrl = 'eval-timeout';
  }

  try {
    logText = await Promise.race([
      client.evaluate('document.getElementById("log")?.textContent?.substring(0, 2000) || "no-log"'),
      new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 5000))
    ]);
  } catch(e) {
    logText = 'eval-timeout';
  }

  const short = (logText || '').replace(/\n/g, ' | ');
  console.log(`  [${(i+1)*2}s] url=${curUrl} log=${short.substring(0, 200)}`);

  // Drain console events
  for (const e of client.events) {
    if (e.method === 'Runtime.consoleAPICalled') {
      const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
      console.log(`  [console] ${text.substring(0, 400)}`);
    }
  }
  client.events.length = 0;

  if (logText?.includes('DONE') || logText?.includes('ERROR')) {
    console.log('\n>>> GOT RESULT');
    break;
  }
}

console.log('Final state captured');

electronProc.kill();
xvfb.kill();
viteProc.kill();
console.log('done');
