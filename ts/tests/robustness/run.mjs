#!/usr/bin/env node
/**
 * Robustness gate: no corrupt image may hang or crash the wasm sandbox.
 *   node ts/tests/robustness/run.mjs --backend wasm|native [--only <glob>[,…]] [--jobs N] [--loglevel N]
 * Generates the corpus (a no-op when current), fetches the syzbot set, runs every case in
 * its own case-runner.mjs process, writes
 * ~/.cache/anyfs-robustness/report-<backend>[-partial].json and prints a
 * summary. Exit: 1 when the wasm gate fails; 2 when setup failed or the
 * harness is broken (a case failed before touching its image).
 */
import { createHash } from 'node:crypto';
import { fork, spawnSync } from 'node:child_process';
import {
    createWriteStream,
    existsSync,
    mkdirSync,
    readFileSync,
    readdirSync,
    renameSync,
    writeFileSync,
} from 'node:fs';
import { availableParallelism } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';
import { fetchSyzbot } from './fetch-syzbot.mjs';
import { caseRecord, gate, harnessFailures } from './lib/classify.mjs';
import { matcher } from './lib/glob.mjs';
import {
    CASES_JSON,
    CORE_DIST,
    LOG_DIR,
    NATIVE_ADDON,
    ROBUSTNESS_DIR,
    WASM_NODE_BUNDLE,
    reportPath,
} from './lib/paths.mjs';
import { diffRuns, formatSummary } from './lib/report.mjs';

/** Per-case outer bound: past it the op watchdog failed to catch a wedge. */
const CASE_TIMEOUT_MS = 3 * 60_000;
/** After an outcome, how long a child may take to exit before it is killed. */
const EXIT_GRACE_MS = 15_000;

function die(msg) {
    console.error(msg);
    process.exit(2);
}

const { values: opts } = parseArgs({
    options: {
        backend: { type: 'string' },
        only: { type: 'string' },
        loglevel: { type: 'string', default: '4' },
        jobs: {
            type: 'string',
            default: String(Math.max(1, Math.min(4, availableParallelism() >> 1))),
        },
    },
});
const backend = opts.backend;
if (backend !== 'wasm' && backend !== 'native') {
    die('usage: run.mjs --backend wasm|native [--only <glob>[,<glob>…]] [--jobs N]');
}
const jobs = Number.parseInt(opts.jobs, 10);
if (!(jobs >= 1)) die(`--jobs: expected a positive integer, got ${opts.jobs}`);
const loglevel = Number.parseInt(opts.loglevel, 10);
if (!(loglevel >= 0)) die(`--loglevel: expected a non-negative integer, got ${opts.loglevel}`);
const only = opts.only ? matcher(opts.only) : null;

const need = [[join(CORE_DIST, 'index.js'), 'pnpm -C ts -F @anyfs/core build']];
if (backend === 'wasm')
    need.push([WASM_NODE_BUNDLE, 'ANYFS_TARGET=node scripts/build_anyfs_wasm.sh']);
else need.push([NATIVE_ADDON, 'ts/packages/anyfs-native/scripts/build-linux-electron.sh']);
for (const [file, how] of need)
    if (!existsSync(file)) die(`missing ${file} — build it with: ${how}`);

// 1. The generated corpus. The generator decides what needs regenerating.
{
    const r = spawnSync(process.execPath, [join(ROBUSTNESS_DIR, 'corpus/generate.mjs')], {
        stdio: 'inherit',
    });
    if (r.status !== 0) die('corpus generation failed');
}
const generated = JSON.parse(readFileSync(CASES_JSON, 'utf-8')).cases;

// 2. The syzbot set.
let syzbot;
try {
    syzbot = await fetchSyzbot({ only });
} catch (e) {
    die(`syzbot fetch failed: ${e.message}`);
}

// 3. One child per case.
const cases = [...generated, ...syzbot].filter((c) => !only || only(c.name));
if (cases.length === 0) die(`no case matches --only ${opts.only}`);
mkdirSync(join(LOG_DIR, backend), { recursive: true });

function runCase(c) {
    return new Promise((resolve) => {
        const t0 = Date.now();
        const logFile = join(LOG_DIR, backend, `${c.name}.log`);
        const log = createWriteStream(logFile);
        let lastStep = 'spawn';
        let outcome = null;
        let timedOut = false;
        let killedAfterOutcome = false;
        let grace = null;
        let timer = null;
        let settled = false;
        const finish = (code, signal, spawnError) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            clearTimeout(grace);
            log.end();
            const rec = caseRecord(
                c,
                { outcome, timedOut, lastStep, code, signal, killedAfterOutcome },
                backend,
            );
            rec.durationMs = Date.now() - t0;
            rec.log = logFile;
            if (spawnError) rec.reason = `spawn failed: ${spawnError.message}`;
            resolve(rec);
        };
        log.on('error', () => {}); // a lost log must not kill the run
        const child = fork(
            join(ROBUSTNESS_DIR, 'case-runner.mjs'),
            ['--backend', backend, '--image', c.file, '--loglevel', String(loglevel)],
            { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] },
        );
        child.on('error', (e) => {
            if (child.pid === undefined || !outcome) {
                child.kill('SIGKILL');
                finish(null, null, e);
            } else {
                log.write(`\n[run.mjs] child error after outcome: ${e.message}\n`);
            }
        });
        child.stdout?.pipe(log, { end: false });
        child.stderr?.pipe(log, { end: false });
        timer = setTimeout(() => {
            timedOut = true;
            child.kill('SIGKILL');
        }, CASE_TIMEOUT_MS);
        child.on('message', (m) => {
            if (m?.type === 'step') {
                lastStep = m.step;
            } else if (m?.type === 'outcome' && !outcome) {
                outcome = m;
                clearTimeout(timer);
                grace = setTimeout(() => {
                    killedAfterOutcome = true;
                    child.kill('SIGKILL');
                }, EXIT_GRACE_MS);
            }
        });
        child.on('close', (code, signal) => finish(code, signal));
    });
}

async function pool(items, n, fn) {
    const out = new Array(items.length);
    let next = 0;
    const worker = async () => {
        while (next < items.length) {
            const i = next++;
            out[i] = await fn(items[i]);
        }
    };
    await Promise.all(Array.from({ length: Math.min(n, items.length) }, worker));
    return out;
}

let done = 0;
const records = await pool(cases, jobs, async (c) => {
    const r = await runCase(c);
    done++;
    console.log(
        `[${String(done).padStart(3)}/${cases.length}] ${r.class.padEnd(5)} ${c.name.padEnd(35)} ${String(r.lastStep).padEnd(12)} ${(r.durationMs / 1000).toFixed(1)}s`,
    );
    return r;
});

const full = reportPath(backend);
let prev = null;
try {
    prev = JSON.parse(readFileSync(full, 'utf-8'));
} catch {
    prev = null; // missing or corrupt: nothing to compare with
}
const sh = (cmd, a) =>
    spawnSync(cmd, a, { encoding: 'utf-8', cwd: ROBUSTNESS_DIR }).stdout?.trim() || null;
const coreJs = readdirSync(CORE_DIST)
    .filter((f) => f.endsWith('.js'))
    .sort()
    .map((f) => join(CORE_DIST, f));
const engineFiles =
    backend === 'wasm'
        ? [WASM_NODE_BUNDLE.replace(/\.mjs$/, '.wasm'), WASM_NODE_BUNDLE, ...coreJs]
        : [NATIVE_ADDON, ...coreJs];
const engineHash = createHash('sha256');
for (const f of engineFiles) engineHash.update(readFileSync(f));
// head is informational; engine (wasm/addon plus @anyfs/core dist) decides "build changed".
const build = {
    head: sh('git', ['rev-parse', '--short', 'HEAD']),
    engine: engineHash.digest('hex').slice(0, 16),
};
const flips = diffRuns(prev, records, build);
const verdict = gate(records, backend);
const broken = harnessFailures(records);
const out = reportPath(backend, only !== null);
writeFileSync(
    `${out}.tmp`,
    `${JSON.stringify({ backend, build, finishedAt: new Date().toISOString(), partial: only !== null, records }, null, 4)}\n`,
);
renameSync(`${out}.tmp`, out);
console.log(formatSummary({ backend, records, verdict, flips, partial: only !== null }));
console.log(`\nreport: ${out}\nlogs:   ${join(LOG_DIR, backend)}`);
if (broken.length > 0) {
    console.error(
        `\nHARNESS BROKEN: these cases failed before touching the image:\n${broken.map((r) => `  ${r.name}: ${r.reason}`).join('\n')}`,
    );
    process.exit(2);
}
process.exit(backend === 'wasm' && !verdict.pass ? 1 : 0);
