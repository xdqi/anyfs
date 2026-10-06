import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { TS_ROOT } from '../lib/paths';
import type { Fixture } from './manifest';

const ROBUSTNESS_DIR = resolve(TS_ROOT, 'tests/robustness');
const CACHE_DIR =
    process.env.ANYFS_ROBUSTNESS_DIR ??
    join(process.env.XDG_CACHE_HOME ?? join(homedir(), '.cache'), 'anyfs-robustness');

/** A generated robustness-corpus case (ts/tests/robustness) as an E2E fixture.
 *  generate.mjs is idempotent (it checks its own fingerprint), so it always runs. */
export function robustnessCase(name: string): Fixture {
    execFileSync(process.execPath, [join(ROBUSTNESS_DIR, 'corpus/generate.mjs')], {
        stdio: 'inherit',
    });
    const casesJson = join(CACHE_DIR, 'generated', 'cases.json');
    const { cases } = JSON.parse(readFileSync(casesJson, 'utf-8')) as {
        cases: { name: string; file: string }[];
    };
    const c = cases.find((x) => x.name === name);
    if (!c) throw new Error(`robustness case ${name} is not in ${casesJson}`);
    return { name, source: 'generated', file: c.file, parts: [] };
}
