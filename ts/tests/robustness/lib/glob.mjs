/** `--only` patterns: comma-separated globs (`*`, `?`) over case names. */
export function matcher(patterns) {
    const res = patterns
        .split(',')
        .map((p) => p.trim())
        .filter(Boolean)
        .map(
            (p) =>
                new RegExp(
                    `^${p
                        .replace(/[.+^${}()|[\]\\]/g, '\\$&')
                        .replace(/\*/g, '.*')
                        .replace(/\?/g, '.')}$`,
                ),
        );
    return (name) => res.some((re) => re.test(name));
}
