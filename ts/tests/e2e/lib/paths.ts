import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));

/** Repo paths relative to ts/tests/e2e/lib. */
export const E2E_DIR = resolve(here, '..');
export const TS_ROOT = resolve(E2E_DIR, '../..');
export const VITE_DEMO_DIR = resolve(TS_ROOT, 'examples/vite-demo');
export const ELECTRON_DEMO_DIR = resolve(TS_ROOT, 'examples/electron-demo');
export const IMAGES_DIR = resolve(E2E_DIR, 'fixtures/images');
