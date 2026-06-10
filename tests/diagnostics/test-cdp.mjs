#!/usr/bin/env node
/*
 * test-cdp.mjs — CDP-based UI test for anyfs reader.
 *
 * Simulates user clicks/typing through CDP to open a disk image and verify
 * the file tree appears. Covers all 6 target×source combinations.
 *
 * DEMOTED to manual diagnostics (see tests/diagnostics/README.md): the
 * Playwright suite at ts/tests/e2e is the primary regression gate.
 *
 * Usage:
 *   node tests/diagnostics/test-cdp.mjs --target electron --source local  [--image ...]
 *   node tests/diagnostics/test-cdp.mjs --target electron --source http   [--image ...]
 *   node tests/diagnostics/test-cdp.mjs --target web      --source local  [--image ...]
 *   node tests/diagnostics/test-cdp.mjs --target web      --source http   [--image ...]
 *
 * For Electron targets, --mode native (default) and --mode wasm control
 * whether the native addon or WASM worker is used. Web targets always use
 * WASM (they have no native addon).
 */

import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, execSync } from 'node:child_process';
import {
  GREEN, RED, BOLD, RST,
  findChrome, startChromium, startVite, startElectron,
  startHttpServer, connectCDP, waitForKernel,
  typeUrl, clickOpenImage, clickFirstPartition, waitForFileTree, findImage, checkJsErrors,
} from '../common.mjs';
import { sleep } from './common-cdp.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));

// ── args ─────────────────────────────────────────────────────────────────

function parseArgs() {
  const a = process.argv;
  const get = (name, def) => {
    const eqIdx = a.findIndex(x => x.startsWith(`${name}=`));
    if (eqIdx >= 0) return a[eqIdx].split('=')[1];
    const spaceIdx = a.indexOf(name);
    if (spaceIdx >= 0 && spaceIdx + 1 < a.length && !a[spaceIdx + 1].startsWith('-')) {
      return a[spaceIdx + 1];
    }
    return def;
  };
  const target = get('--target', 'web');
  const source = get('--source', 'local');
  const mode = get('--mode', 'native');
  const externalUrl = get('--url', null);
  const imagePath = source === 'http' && externalUrl ? externalUrl : findImage();
  const cdpPort = 9260 + Math.floor(Math.random() * 30);
  return { target, source, mode, imagePath, externalUrl, cdpPort };
}

// ── test runner ──────────────────────────────────────────────────────────

let passed = 0;
let failed = 0;
function pass(msg) { passed++; console.log(`  ${GREEN}✓${RST} ${msg}`); }
function fail(msg) { failed++; console.log(`  ${RED}✗${RST} ${msg}`); process.exitCode = 1; }
function assert(cond, msg) { cond ? pass(msg) : fail(msg); }

async function main() {
  // Kill lingering processes from previous test runs so port 5173 is free
  // (Electron main.ts hardcodes http://localhost:5173/ in dev mode).
  try { execSync('pkill -f "vite"', { stdio: 'ignore' }); } catch {}
  try { execSync('pkill -f "electron-demo"', { stdio: 'ignore' }); } catch {}
  try { execSync('pkill -f "Xvfb.*:9"', { stdio: 'ignore' }); } catch {}
  await sleep(1000);

  const { target, source, mode, imagePath, externalUrl, cdpPort } = parseArgs();
  const label = `${target}-${mode}-${source}`;
  const displayPath = externalUrl || imagePath;
  console.log(`${BOLD}[test-cdp ${label}]${RST} source: ${displayPath}`);

  let httpServer = null;
  let viteProc = null;
  let chromeProc = null;
  let electronProc = null;
  let electronXvfb = null;

  try {
    // ── Phase 1: start target ──────────────────────────────────────────
    if (target === 'electron') {
      // Start vite dev server for Electron to load from
      console.log('  starting vite dev server...');
      const { proc, url } = await startVite();
      viteProc = proc;
      console.log(`  vite: ${url}`);

      // Build env for Electron
      /** @type {Record<string,string>} */
      const extraEnv = { ELECTRON_DEV: '1' };
      if (mode === 'wasm') extraEnv.ANYFS_DISABLE_NATIVE = '1';
      if (source === 'local') {
        extraEnv.ANYFS_TEST_LOCAL_PATH = imagePath;
      }

      console.log(`  starting Electron (mode=${mode})...`);
      const { proc: eproc, xvfb } = startElectron(cdpPort, extraEnv);
      electronProc = eproc;
      electronXvfb = xvfb;
      await sleep(4000);
    } else {
      // Web target
      console.log('  starting vite dev server...');
      const { proc, url } = await startVite();
      viteProc = proc;
      console.log(`  vite: ${url}`);

      console.log('  starting headless Chromium...');
      chromeProc = startChromium(cdpPort, url);
      await sleep(3000);
    }

    // ── Phase 2: connect CDP ───────────────────────────────────────────
    const { client, page } = await connectCDP('127.0.0.1', cdpPort);
    console.log(`  page: ${page.title} @ ${page.url}`);
    assert(page.title.includes('anyfs') || page.title.includes('anyfs'),
      `page title contains "anyfs" (got: "${page.title}")`);

    // ── Phase 3: wait for kernel ───────────────────────────────────────
    const bootStatus = await waitForKernel(client);
    if (bootStatus === 'boot-log') pass('kernel booted (dmesg in console)');
    else if (bootStatus === 'coi-ready') pass('kernel appears ready (crossOriginIsolated)');
    else fail('kernel did not boot');

    // ── Phase 4: trigger disk open ─────────────────────────────────────
    if (source === 'http') {
      let targetUrl;
      if (externalUrl) {
        // External URL:
        //   native mode: raw HTTPS URL — proxy worker uses Node.js fetch (no CORS)
        //   wasm mode (electron): anyfs-url://proxy/ wrapper to bypass CORS
        //   web: raw URL (blocked by CORS, skipped by user)
        if (target === 'electron' && mode === 'native') {
          targetUrl = externalUrl;
        } else if (target === 'electron') {
          targetUrl = `anyfs-url://proxy/?u=${encodeURIComponent(externalUrl)}`;
        } else {
          targetUrl = externalUrl;
        }
        console.log(`  external URL: ${targetUrl}`);
      } else {
        // Local file served over HTTP
        console.log('  starting HTTP file server...');
        const { server, port } = await startHttpServer(imagePath);
        httpServer = server;
        const httpUrl = `http://127.0.0.1:${port}/disk`;
        console.log(`  HTTP URL: ${httpUrl}`);
        if (target === 'electron' && mode === 'native') {
          targetUrl = httpUrl;
        } else if (target === 'electron') {
          targetUrl = `anyfs-url://proxy/?u=${encodeURIComponent(httpUrl)}`;
        } else {
          targetUrl = httpUrl;
        }
      }

      // Try the CDP test hook first (bypasses React event issues in Electron),
      // then fall back to UI click/type flow.
      const hookResult = await client.evaluate(`
        (() => {
          if (window.__anyfsTest && window.__anyfsTest.openUrl) {
            try {
              window.__anyfsTest.openUrl(${JSON.stringify(targetUrl)});
              return 'hook-ok';
            } catch(e) { return 'hook-err:' + e.message; }
          }
          return 'no-hook';
        })()
      `);
      console.log(`  openUrl hook: ${hookResult}`);

      if (hookResult !== 'hook-ok') {
        const typed = await typeUrl(client, targetUrl);
        console.log(`  typeUrl: ${typed}`);
        assert(typed === 'ok', `URL typed into input (got: ${typed})`);
      } else {
        pass('opened URL via test hook');
      }
    } else {
      // Local file
      if (target === 'electron' && mode === 'native') {
        // Native: use IPC dialog (returns host path → attachPath).
        const clicked = await clickOpenImage(client);
        console.log(`  clickOpenImage: ${clicked}`);
        if (clicked === 'clicked') {
          pass('clicked Open Image button');
        } else if (clicked && clicked.startsWith('no-btn')) {
          fail(`Open Image button not found: ${clicked}`);
        }
      } else {
        // WASM modes: serve the local file over HTTP and use attachUrl
        // (URLFS).  CDP-injected File objects (DOM.setFileInputFiles) cannot
        // be structured-cloned through postMessage to a Web Worker — the
        // message is silently dropped.  URLFS avoids the File round-trip.
        console.log('  starting HTTP file server (wasm URLFS path)...');
        const { server, port } = await startHttpServer(imagePath);
        httpServer = server;
        const httpUrl = `http://127.0.0.1:${port}/disk`;
        console.log(`  HTTP URL: ${httpUrl}`);

        const hookResult = await client.evaluate(`
          (() => {
            if (window.__anyfsTest && window.__anyfsTest.openUrl) {
              try {
                window.__anyfsTest.openUrl(${JSON.stringify(httpUrl)});
                return 'hook-ok';
              } catch(e) { return 'hook-err:' + e.message; }
            }
            return 'no-hook';
          })()
        `);
        console.log(`  openUrl hook: ${hookResult}`);
        if (hookResult === 'hook-ok') {
          pass('opened local file via URLFS (test hook)');
        } else {
          fail(`openUrl hook failed: ${hookResult}`);
        }
      }
    }

    // Diagnostic: probe page state after openUrl
    await sleep(2000);
    const probe = await client.evaluate(`
      (() => {
        try {
          const body = document.body?.innerText || '(no body)';
          const hasTest = !!window.__anyfsTest;
          return JSON.stringify({ hasTest, body: body.substring(0, 400) });
        } catch(e) { return 'probe-err:' + e.message; }
      })()
    `);
    console.log(`  state probe: ${probe}`);

    // ── Phase 4.5: if a partition list appeared, click the first one ──
    if (source === 'local') {
      await sleep(3000);
      const partResult = await clickFirstPartition(client);
      console.log(`  clickFirstPartition: ${partResult}`);
      if (partResult && partResult.startsWith('clicked:')) {
        pass(`selected partition: ${partResult.replace('clicked: ', '')}`);
        await sleep(2000);
      } else if (partResult === 'no-partition-btn') {
        console.log('  (no partition list — single filesystem mount)');
      }
    }

    // ── Phase 5: wait for file tree ────────────────────────────────────
    console.log('  waiting for file tree...');
    const treeResult = await waitForFileTree(client, 120000);
    if (treeResult === 'text-matched') {
      pass('file content visible in page (text match)');
    } else if (typeof treeResult === 'number' && treeResult > 0) {
      pass(`file tree appeared (${treeResult} entries)`);
    } else {
      // Last chance: dump body text for diagnosis
      const bodyText = await client.evaluate(
        'document.body.innerText.substring(0, 400)');
      console.log(`  page body: ${bodyText.replace(/\n/g, ' ').slice(0, 300)}`);
      fail(`file tree did not appear (got: ${treeResult})`);
    }

    // ── Phase 6: check for JS errors ───────────────────────────────────
    const jsErrs = await checkJsErrors(client);
    if (jsErrs.count > 0) {
      console.log(`  JS errors sample: ${jsErrs.sample}`);
    }
    // JS errors are non-fatal for this test (some are expected in headless)
    if (jsErrs.count === 0) pass('no JS errors');
    else console.log(`  (${jsErrs.count} JS errors — may be headless artifacts)`);

  } finally {
    // ── Cleanup ────────────────────────────────────────────────────────
    console.log('  cleaning up...');
    if (electronProc) {
      electronProc.kill();
      try { electronProc.kill('SIGKILL'); } catch {}
    }
    if (electronXvfb) {
      electronXvfb.kill();
      try { electronXvfb.kill('SIGKILL'); } catch {}
    }
    if (chromeProc) {
      chromeProc.kill();
      try { chromeProc.kill('SIGKILL'); } catch {}
    }
    if (viteProc) {
      viteProc.kill();
      try { viteProc.kill('SIGKILL'); } catch {}
    }
    if (httpServer) httpServer.close();
    await sleep(500);
  }

  console.log(`\n${BOLD}[test-cdp ${label}]${RST} ${passed} pass, ${failed} fail`);
  if (failed > 0) process.exitCode = 1;
}

main().catch(err => {
  console.error(`${RED}FATAL:${RST} ${err.message}`);
  process.exit(1);
});
