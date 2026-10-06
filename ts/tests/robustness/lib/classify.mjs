/** Outcome classes, in report order. */
export const CLASSES = ['ok', 'error', 'fatal', 'hang', 'crash'];

/** What a child that finished its steps reports: fatal wins over errors. */
export function selfClass({ fatal, errors }) {
    if (fatal) return 'fatal';
    return errors.length > 0 ? 'error' : 'ok';
}

/** The parent's verdict on one child: its own outcome if it sent one,
 *  otherwise hang (the outer timeout fired) or crash (it died). */
export function finalClass({ outcome, timedOut }) {
    if (outcome) return outcome.class;
    return timedOut ? 'hang' : 'crash';
}

/**
 * One report record from a case's definition and what its child did. Pure,
 * so every shape (ok, error, fatal, hang, crash, kill after outcome) is
 * unit-tested. `reason` is the child's own for any outcome (null for ok).
 */
export function caseRecord(
    c,
    { outcome, timedOut, lastStep, code, signal, killedAfterOutcome, durationMs = 0, log = null },
    backend,
) {
    let reason;
    if (outcome) reason = outcome.reason ?? null;
    else if (timedOut) reason = 'no outcome within the outer timeout';
    else reason = `exited without an outcome (code ${code}, signal ${signal})`;
    return {
        name: c.name,
        source: c.source,
        fs: c.fs,
        mutation: c.mutation,
        sha256: c.sha256,
        expect: c.expect ?? null,
        backend,
        class: finalClass({ outcome, timedOut }),
        lastStep: outcome?.lastStep ?? lastStep,
        failedStep: outcome ? (outcome.failedStep ?? null) : lastStep,
        durationMs,
        reason,
        errors: outcome?.errors ?? [],
        stats: outcome?.stats ?? null,
        exit: { code, signal, killedAfterOutcome },
        log,
    };
}

/** Cases that failed before the image mattered: the harness is broken. */
export function harnessFailures(records) {
    return records.filter(
        (r) => r.class !== 'ok' && ['spawn', 'start', 'boot'].includes(r.lastStep),
    );
}

/**
 * Gate rules: every unmutated base is ok, every fatal carries a reason, and
 * nothing hangs or crashes. On native these are findings: they are listed,
 * but only a wasm run fails.
 */
export function gate(records, backend) {
    const problems = [];
    for (const r of records) {
        if (r.class === 'hang' || r.class === 'crash') {
            problems.push(`${r.name}: ${r.class} at ${r.lastStep} (${r.reason})`);
        } else if (r.mutation === 'none' && r.class !== 'ok') {
            problems.push(
                `${r.name}: unmutated base must be ok, got ${r.class} (${r.reason ?? '-'})`,
            );
        } else if (r.mutation === 'none' && r.expect) {
            for (const k of ['entries', 'files', 'bytes', 'parts']) {
                const want = r.expect[k];
                if (want !== undefined && r.stats?.[k] !== want) {
                    problems.push(
                        `${r.name}: unmutated base listed ${r.stats?.[k] ?? 'no'} ${k}, expected ${want}`,
                    );
                    break;
                }
            }
        }
        if (r.class === 'fatal' && !r.reason) problems.push(`${r.name}: fatal without a reason`);
    }
    return { pass: backend !== 'wasm' || problems.length === 0, problems };
}
