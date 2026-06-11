#!/usr/bin/env node
/*
 * run-all.mjs — Run all 6 CDP-based UI tests for anyfs reader.
 *
 * DEMOTED to manual diagnostics (see README.md in this directory): the
 * Playwright suite at ts/tests/e2e is the primary regression gate.
 *
 * Usage:
 *   node tests/diagnostics/run-all.mjs [--image path/to/disk.iso]
 *
 * The 6 combinations:
 *   1. web      + WASM   + local file
 *   2. web      + WASM   + HTTP URL
 *   3. electron + WASM   + local file
 *   4. electron + WASM   + HTTP URL
 *   5. electron + native + local file
 *   6. electron + native + HTTP URL
 */

import { spawn } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const TEST_SCRIPT = resolve(__dirname, 'test-cdp.mjs');

const BOLD = '\x1b[1m';
const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const RST = '\x1b[0m';

const suites = [
  { name: 'web-wasm-local',     args: ['--target', 'web', '--source', 'local'] },
  { name: 'web-wasm-http',      args: ['--target', 'web', '--source', 'http'] },
  { name: 'electron-wasm-local',  args: ['--target', 'electron', '--mode', 'wasm', '--source', 'local'] },
  { name: 'electron-wasm-http',   args: ['--target', 'electron', '--mode', 'wasm', '--source', 'http'] },
  { name: 'electron-native-local', args: ['--target', 'electron', '--mode', 'native', '--source', 'local'] },
  { name: 'electron-native-http',  args: ['--target', 'electron', '--mode', 'native', '--source', 'http'] },
];

const imageArg = process.argv.find(a => a.startsWith('--image='));
const extraArgs = imageArg ? [imageArg] : [];

async function runOne(name, args) {
  const allArgs = [TEST_SCRIPT, ...args, ...extraArgs];
  const label = `${BOLD}${name}${RST}`;
  console.log(`\n${'═'.repeat(60)}`);
  console.log(`═══ ${label}`);
  console.log(`${'═'.repeat(60)}`);

  return new Promise((resolve) => {
    const proc = spawn('node', allArgs, { stdio: 'inherit' });
    proc.on('close', (code) => resolve({ name, passed: code === 0 }));
    proc.on('error', (err) => {
      console.error(`${RED}  spawn error: ${err.message}${RST}`);
      resolve({ name, passed: false });
    });
  });
}

async function main() {
  console.log(`${BOLD}[run-all]${RST} 6 test combinations`);
  console.log(`  start: ${new Date().toISOString()}`);
  if (extraArgs.length) console.log(`  extra args: ${extraArgs.join(' ')}`);

  const results = [];
  for (const s of suites) {
    results.push(await runOne(s.name, s.args));
  }

  console.log(`\n${BOLD}═══ Summary ═══${RST}`);
  const maxLen = Math.max(...results.map(r => r.name.length));
  for (const r of results) {
    const status = r.passed ? `${GREEN}PASS${RST}` : `${RED}FAIL${RST}`;
    console.log(`  ${r.name.padEnd(maxLen)}  ${status}`);
  }

  const allPassed = results.every(r => r.passed);
  console.log(`\n  finish: ${new Date().toISOString()}`);
  console.log(`  result: ${allPassed ? `${GREEN}ALL PASS${RST}` : `${RED}SOME FAIL${RST}`}`);
  if (!allPassed) process.exit(1);
}

main().catch(err => {
  console.error(`${RED}FATAL:${RST} ${err.message}`);
  process.exit(1);
});
