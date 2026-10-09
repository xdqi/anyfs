#!/usr/bin/env node
// Print the Electron version electron-demo is locked to (ts/pnpm-lock.yaml),
// so native addons built in jobs that don't install electron-demo use the
// headers and node.lib of the Electron that ships.
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const lock = readFileSync(
    resolve(dirname(fileURLToPath(import.meta.url)), '../../../pnpm-lock.yaml'),
    'utf8',
);
const importer = lock.split(/\n  (?=\S)/).find((s) => s.startsWith('examples/electron-demo:'));
const m = importer?.match(/\n {6}electron:\n {8}specifier: .*\n {8}version: (\d+\.\d+\.\d+)/);
if (!m) {
    console.error('electron-version: electron not found in the electron-demo importer');
    process.exit(1);
}
console.log(m[1]);
