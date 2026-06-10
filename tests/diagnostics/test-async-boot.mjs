#!/usr/bin/env node
// Test async boot: Electron → anyfs.worker.js → anyfs.workeronly.mjs (async boot)
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
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

// Clean up any leftover processes
try { process.kill(parseInt(fs.readFileSync('/tmp/xvfb_96.pid','utf8'))); } catch(e) {}

const viteDir = resolve(__dirname, '../../ts/examples/vite-demo');
const viteProc = spawn('npx', ['vite', '--port', '5204'], {
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

const xvfb = spawn('Xvfb', [':96', '-screen', '0', '1280x720x24'], { stdio: 'ignore' });
xvfb.pid && fs.writeFileSync('/tmp/xvfb_96.pid', String(xvfb.pid));
await sleep(500);

const cdpPort = 9404;
const electronDir = resolve(__dirname, '../../ts/examples/electron-demo');
const electronBin = resolve(electronDir, 'node_modules/.bin/electron');
const electronProc = spawn(electronBin, [
  electronDir, `--remote-debugging-port=${cdpPort}`
], {
  cwd: electronDir,
  stdio: ['ignore', 'pipe', 'pipe'],
  env: { ...process.env, DISPLAY: ':96', ELECTRON_DEV: '1', ANYFS_DISABLE_NATIVE: '1' }
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

// Navigate to debug-worker.html
console.log('navigating to debug-worker.html...');
await client.send('Page.navigate', { url: viteUrl + '/debug-worker.html' });
await sleep(3000);

// Drain startup console events
for (const e of client.events) {
  if (e.method === 'Runtime.consoleAPICalled') {
    const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
    console.log(`  [console-startup] ${text.substring(0, 400)}`);
  }
}
client.events.length = 0;

// Poll for result
let hasResult = false;
for (let i = 0; i < 90; i++) {
  await sleep(1000);

  let logText = 'eval-failed';
  try {
    logText = await Promise.race([
      client.evaluate('document.getElementById("log")?.textContent || "no-log"'),
      new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 5000))
    ]);
  } catch(e) {}

  // Drain console events
  for (const e of client.events) {
    if (e.method === 'Runtime.consoleAPICalled') {
      const text = e.params?.args?.map(a => a.value ?? '').join(' ') || '';
      console.log(`  [console] ${text.substring(0, 400)}`);
    }
  }
  client.events.length = 0;

  const short = (logText || '').replace(/\n/g, ' | ');
  if (short.length > 300) {
    console.log(`  [${i+1}s] ${short.substring(0, 150)}...${short.substring(short.length - 150)}`);
  } else {
    console.log(`  [${i+1}s] ${short}`);
  }

  if (logText?.includes('"ok":true') || logText?.includes('boot_result=0')) {
    console.log('\n>>> SUCCESS');
    hasResult = true;
    break;
  }
  if (logText?.includes('BOOT ERROR') || logText?.includes('WORKER ERROR') || logText?.includes('TIMEOUT') || logText?.includes('boot_result=-')) {
    console.log('\n>>> FAILURE');
    hasResult = true;
    break;
  }
}
if (!hasResult) console.log('\n>>> NO RESULT after 90s');

electronProc.kill();
xvfb.kill();
viteProc.kill();
console.log('done');
