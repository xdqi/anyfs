#!/usr/bin/env node
// Minimal test: navigate Electron to debug-worker.html, watch for prewarm result.
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, execSync } from 'node:child_process';
import { startVite, startElectron, connectCDP, GREEN, RED, BOLD, RST } from './common.mjs';
import { sleep } from './diagnostics/common-cdp.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));

try { execSync('pkill -9 -f "vite"', { stdio: 'ignore' }); } catch {}
try { execSync('pkill -9 -f "electron"', { stdio: 'ignore' }); } catch {}
try { execSync('pkill -9 -f "Xvfb"', { stdio: 'ignore' }); } catch {}
await sleep(1500);

console.log('starting vite...');
const { proc: viteProc, url: viteUrl } = await startVite();
console.log('vite:', viteUrl);

const cdpPort = 9370 + Math.floor(Math.random() * 30);
console.log('starting Electron...');
const { proc: electronProc, xvfb } = startElectron(cdpPort, {
  ELECTRON_DEV: '1',
  ANYFS_DISABLE_NATIVE: '1',
});
await sleep(4000);

console.log('connecting CDP...');
const { client, page } = await connectCDP('127.0.0.1', cdpPort);
console.log('page:', page.title, '@', page.url);

// Navigate to debug page
console.log('navigating to debug-worker.html...');
await client.evaluate(`window.location.href = ${JSON.stringify(viteUrl + '/debug-worker.html')}`);
await sleep(2000);

// Verify we're on the right page
const currentUrl = await client.evaluate('window.location.href');
console.log('current URL:', currentUrl);

// Watch for result
const deadline = Date.now() + 35000;
while (Date.now() < deadline) {
  await sleep(1000);
  const elapsed = Math.round((Date.now() - (deadline - 35000)) / 1000);

  const logText = await client.evaluate(
    'document.getElementById("log")?.innerText?.substring(0, 600) || "no-log"'
  );
  console.log(`  [${elapsed}s] ${(logText || '').replace(/\n/g, ' | ')}`);

  if (logText.includes('prewarm OK') || logText.includes('ERROR')) {
    console.log(`\n${BOLD}RESULT:${RST} ${logText.replace(/\n/g, ' ')}`);
    break;
  }
}

// Cleanup
electronProc.kill();
try { electronProc.kill('SIGKILL'); } catch {}
xvfb.kill();
try { xvfb.kill('SIGKILL'); } catch {}
viteProc.kill();
try { viteProc.kill('SIGKILL'); } catch {}
console.log('DONE');
