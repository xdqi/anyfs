import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { TS_ROOT } from '../lib/paths';
import type { Fixture } from './manifest';

const ROBUSTNESS_DIR = resolve(TS_ROOT, 'tests/robustness');
const CACHE_DIR =
    process.env.ANYFS_ROBUSTNESS_DIR ??
    join(process.env.XDG_CACHE_HOME ?? join(homedir(), '.cache'), 'anyfs-robustness');

/** A robustness-corpus case (ts/tests/robustness) as an E2E fixture.
 *  Generates the corpus, or fetches the one syzbot image, on first use. */
export function robustnessCase(name: string): Fixture {
    const file = name.startsWith('syz-') ? syzbotFile(name) : generatedFile(name);
    return { name, source: 'generated', file, parts: [] };
}

function generatedFile(name: string): string {
    const casesJson = join(CACHE_DIR, 'generated', 'cases.json');
    if (!existsSync(casesJson)) {
        execFileSync(process.execPath, [join(ROBUSTNESS_DIR, 'corpus/generate.mjs')], {
            stdio: 'inherit',
        });
    }
    const { cases } = JSON.parse(readFileSync(casesJson, 'utf-8')) as {
        cases: { name: string; file: string }[];
    };
    const c = cases.find((x) => x.name === name);
    if (!c) throw new Error(`robustness case ${name} is not in ${casesJson}`);
    return c.file;
}

function syzbotFile(name: string): string {
    const out = execFileSync(
        process.execPath,
        [join(ROBUSTNESS_DIR, 'fetch-syzbot.mjs'), '--only', name],
        { encoding: 'utf-8' },
    );
    const line = out.split('\n').find((l) => l.startsWith(`${name}\t`));
    if (!line) throw new Error(`syzbot case ${name} is not in syzbot.json`);
    return line.split('\t')[1]!;
}
