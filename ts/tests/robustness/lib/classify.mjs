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
        if (r.mutation === 'none' && r.class !== 'ok') {
            problems.push(
                `${r.name}: unmutated base must be ok, got ${r.class} (${r.reason ?? '-'})`,
            );
        }
        if (r.class === 'fatal' && !r.reason) problems.push(`${r.name}: fatal without a reason`);
        if (r.class === 'hang' || r.class === 'crash') {
            problems.push(`${r.name}: ${r.class} at ${r.lastStep} (${r.reason})`);
        }
    }
    return { pass: backend !== 'wasm' || problems.length === 0, problems };
}
