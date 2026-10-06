import { CLASSES } from './classify.mjs';

export function counts(records) {
    const c = Object.fromEntries(CLASSES.map((k) => [k, 0]));
    for (const r of records) c[r.class]++;
    return c;
}

/** Cases whose class changed since `prev` on the same image bytes — a
 *  finding in itself, since the corpus is deterministic. */
export function diffRuns(prev, records, build) {
    if (!prev) return [];
    const was = new Map(prev.records.map((r) => [r.name, r]));
    return records
        .filter((r) => {
            const p = was.get(r.name);
            return p && p.sha256 === r.sha256 && p.class !== r.class;
        })
        .map((r) => {
            const f = { name: r.name, was: was.get(r.name).class, now: r.class };
            if (build) {
                f.buildChanged =
                    prev.build?.head !== build.head || prev.build?.bundle !== build.bundle;
            }
            return f;
        });
}

export function formatSummary({ backend, records, verdict, flips, partial }) {
    const c = counts(records);
    const lines = [
        '',
        `=== robustness: ${backend}, ${records.length} cases${partial ? ' (PARTIAL RUN via --only: not a gate result)' : ''} ===`,
        CLASSES.map((k) => `${k} ${c[k]}`).join('   '),
    ];
    const notable = records.filter((r) => ['fatal', 'hang', 'crash'].includes(r.class));
    if (notable.length > 0) {
        lines.push('', 'class  case                                failed step   reason');
        for (const r of notable) {
            lines.push(
                `${r.class.padEnd(6)} ${r.name.padEnd(35)} ${String(r.failedStep ?? r.lastStep).padEnd(13)} ${r.reason ?? ''}`,
            );
        }
    }
    const killed = records.filter((r) => r.exit?.killedAfterOutcome);
    if (killed.length > 0) {
        lines.push(
            '',
            'note: killed after reporting an outcome (the child did not exit on its own):',
        );
        for (const r of killed) lines.push(`  ${r.name}`);
    }
    if (flips.length > 0) {
        lines.push('', 'class changed since the previous run (a finding):');
        for (const f of flips)
            lines.push(
                `  ${f.name}: ${f.was} → ${f.now}${f.buildChanged ? ' (build changed)' : ''}`,
            );
    }
    if (verdict.problems.length > 0) {
        lines.push(
            '',
            backend === 'wasm'
                ? 'GATE FAILED:'
                : 'findings (non-gating on native — record them in ts/tests/robustness/FINDINGS.md):',
        );
        for (const p of verdict.problems) lines.push(`  ${p}`);
    } else {
        lines.push(
            '',
            backend === 'wasm' ? 'gate passed' : 'no native hangs, crashes or base failures',
        );
    }
    return lines.join('\n');
}
