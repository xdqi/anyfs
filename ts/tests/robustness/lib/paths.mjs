/** Paths shared by the robustness corpus, harness and tests. */
import { homedir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROBUSTNESS_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '..');
export const TS_DIR = resolve(ROBUSTNESS_DIR, '../..');

/** Everything generated or downloaded lives here — never /tmp (a small tmpfs). */
export const CACHE_DIR =
    process.env.ANYFS_ROBUSTNESS_DIR ??
    join(process.env.XDG_CACHE_HOME ?? join(homedir(), '.cache'), 'anyfs-robustness');
export const GENERATED_DIR = join(CACHE_DIR, 'generated');
export const CASES_JSON = join(GENERATED_DIR, 'cases.json');
export const SYZBOT_DIR = join(CACHE_DIR, 'syzbot');
export const LOG_DIR = join(CACHE_DIR, 'logs');
export const SCRATCH_DIR = join(CACHE_DIR, 'test-scratch');

export const SYZBOT_MANIFEST = join(ROBUSTNESS_DIR, 'syzbot.json');
export const CORE_DIST = join(TS_DIR, 'packages/core/dist');
export const WASM_NODE_BUNDLE = join(TS_DIR, 'packages/core/wasm/anyfs.node.mjs');
export const NATIVE_ADDON = join(TS_DIR, 'packages/anyfs-native/build/Release/anyfs_native.node');

/** report-<backend>.json for a full run; a --only run writes -partial. */
export const reportPath = (backend, partial = false) =>
    join(CACHE_DIR, `report-${backend}${partial ? '-partial' : ''}.json`);
