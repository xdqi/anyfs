#!/usr/bin/env node
/**
 * Runs one robustness case in a fresh process (run.mjs forks one per case):
 *   node case-runner.mjs --backend wasm|native --image <path> [--loglevel N]
 * Steps: boot, open, listParts, then per partition enter / walk / read, then
 * close. Sends {type:'step', step} as it goes and ends with exactly one
 * {type:'outcome', class, lastStep, failedStep, reason, errors, stats}
 * (failedStep: where the first error happened, e.g. enter:0, walk:1, read:0;
 * a fatal's is its lastStep. A fatal outcome carries stats and errors only
 * from before the partition walk that went fatal); run.mjs turns a
 * missing outcome into hang (timed out) or crash (died). Without an IPC
 * channel the messages go to stdout as JSON lines, for debugging one case.
 */
import { basename, dirname } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { NativeSession, NodeWasmSession } from '../../packages/core/dist/index.js';
import { bootNodeKernel } from '../../packages/core/dist/node.js';
import { selfClass } from './lib/classify.mjs';
import { nativeBridge } from './lib/native-bridge.mjs';
import { WASM_NODE_BUNDLE } from './lib/paths.mjs';
import { walkAndRead } from './lib/walk.mjs';

/** Per-op watchdog in the harness; the product default is 60 s. */
const OP_TIMEOUT_MS = 20_000;
/** The provider's attach bound (DEFAULT_ATTACH_TIMEOUT_MS in provider.tsx). */
const ATTACH_TIMEOUT_MS = 120_000;
/** Booting never reads the image; past this the harness itself is broken. */
const BOOT_TIMEOUT_MS = 60_000;
const ANYFS_MOUNT_RDONLY = 1;
/** Container slots are not filesystems; DiskView doesn't offer them either. */
const CONTAINER_KINDS = new Set(['NESTED', 'LVM_PV', 'LUKS']);

const { values: args } = parseArgs({
    options: {
        backend: { type: 'string' },
        image: { type: 'string' },
        loglevel: { type: 'string', default: '4' },
    },
});
if (!['wasm', 'native'].includes(args.backend) || !args.image) {
    console.error('usage: case-runner.mjs --backend wasm|native --image <path>');
    process.exit(2);
}

const LOGLEVEL = Number.parseInt(args.loglevel, 10);
const errors = [];
const stats = { parts: 0, entries: 0, files: 0, bytes: 0 };
let lastStep = 'start';
let fatal = null;
let reported = false;

const message = (e) => (e instanceof Error ? e.message : String(e));
const emit = (m) =>
    process.send
        ? new Promise((resolve) => process.send(m, () => resolve()))
        : Promise.resolve(console.log(JSON.stringify(m)));

function step(name) {
    lastStep = name;
    void emit({ type: 'step', step: name });
}

async function report(cls, reason) {
    if (reported) return;
    reported = true;
    const failedStep = cls === 'fatal' ? lastStep : (errors[0]?.step ?? null);
    await emit({
        type: 'outcome',
        class: cls,
        lastStep,
        failedStep,
        reason,
        errors: errors.slice(0, 20),
        stats,
    });
    // A piped child loses unflushed output on exit; the kernel log matters.
    await new Promise((r) => process.stdout.write('', r));
    await new Promise((r) => process.stderr.write('', r));
    process.exit(0);
}

function onFatal(err) {
    if (fatal) return;
    fatal = err;
    void report('fatal', message(err));
}

if (args.backend === 'wasm') {
    // Mirror the browser worker: there an uncaught error or rejection on the
    // module-owning thread becomes a session fatal (worker.ts posts
    // host-error / host-rejection, WasmSession fires onFatal). Node has no
    // worker around the module, and emscripten rethrows a pthread's abort on
    // this thread right after onAbort.
    process.on('uncaughtException', (e) => onFatal(new Error(`host-error: ${message(e)}`)));
    process.on('unhandledRejection', (e) => onFatal(new Error(`host-rejection: ${message(e)}`)));
}

/** Reject after `ms` with an error marked timedOut. */
function within(ms, what, p) {
    let timer;
    const t = new Promise((_, reject) => {
        timer = setTimeout(
            () =>
                reject(
                    Object.assign(new Error(`${what} timed out after ${ms / 1000}s`), {
                        timedOut: true,
                    }),
                ),
            ms,
        );
    });
    return Promise.race([p, t]).finally(() => clearTimeout(timer));
}

async function open() {
    step('boot');
    let session;
    if (args.backend === 'wasm') {
        const { default: factory } = await import(pathToFileURL(WASM_NODE_BUNDLE).href);
        const M = await within(
            BOOT_TIMEOUT_MS,
            'boot',
            bootNodeKernel(dirname(args.image), factory, { loglevel: LOGLEVEL }),
        );
        session = new NodeWasmSession(M, { opTimeoutMs: OP_TIMEOUT_MS, readOnly: true });
        session.onFatal(onFatal);
        step('open');
        await within(
            ATTACH_TIMEOUT_MS,
            'attach',
            session.attachPath(`/work/${basename(args.image)}`),
        );
    } else {
        session = new NativeSession(nativeBridge(), { opTimeoutMs: OP_TIMEOUT_MS });
        session.onFatal(onFatal);
        await within(BOOT_TIMEOUT_MS, 'boot', session.boot(256, LOGLEVEL));
        step('open');
        await within(ATTACH_TIMEOUT_MS, 'attach', session.attachPath(args.image));
    }
    return session;
}

async function main() {
    const session = await open();
    step('listParts');
    const parts = await session.listParts();
    const targets = parts.filter((p) => !CONTAINER_KINDS.has(p.kind)).map((p) => p.index);
    // No partition table: enter the whole disk, as DiskView's #0 does.
    if (targets.length === 0) targets.push(0);
    for (const idx of targets) {
        if (fatal) return;
        step(`enter:${idx}`);
        let mountPoint;
        try {
            mountPoint = await session.enter(idx, ANYFS_MOUNT_RDONLY);
        } catch (e) {
            errors.push({
                op: 'enter',
                path: `#${idx}`,
                message: message(e),
                step: `enter:${idx}`,
            });
            continue;
        }
        stats.parts++;
        const r = await walkAndRead(session, mountPoint, {
            stopped: () => fatal !== null,
            onStep: (s) => step(`${s}:${idx}`),
        });
        stats.entries += r.entries;
        stats.files += r.files;
        stats.bytes += r.bytes;
        errors.push(...r.errors.map((e) => ({ ...e, step: `${e.step}:${idx}` })));
    }
    if (fatal) return;
    step('close');
    await session.close();
}

main().then(
    () => {
        if (fatal) return;
        const first = errors[0];
        void report(
            selfClass({ fatal, errors }),
            first ? `${first.op} ${first.path}: ${first.message}` : null,
        );
    },
    (e) => {
        if (fatal) return;
        // Attach past the provider's bound: the app tears the worker down and
        // shows an error. The session is gone, so this counts as a fatal.
        if (e?.timedOut && lastStep === 'open') {
            onFatal(e);
            return;
        }
        // A trap on the module-owning thread bricks the module: a fatal.
        if (args.backend === 'wasm' && e instanceof WebAssembly.RuntimeError) {
            onFatal(e);
            return;
        }
        errors.push({ op: lastStep, path: args.image, message: message(e), step: lastStep });
        void report('error', `${lastStep}: ${message(e)}`);
    },
);
