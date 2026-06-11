# shellcheck shell=bash
# scripts/lib/wasm_exports.sh — derive -sEXPORTED_FUNCTIONS for the wasm
# bundle from the TS glue source. ts/native/anyfs_ts.c is the single source
# of truth: every non-static anyfs_ts_* function defined at column 0 is
# exported (renaming a glue function can therefore never silently drop it
# from the bundle — the failure surfaces in the node smoke test instead).
# The DEF_P_TRAMP(name, ...) out-pointer trampolines token-paste their
# names (anyfs_ts_<name>_p), so macro invocations are parsed as well.
anyfs_wasm_exports() {
    local glue="$1" syms s out
    syms="$({ grep -hE '^[A-Za-z_][A-Za-z0-9_* ]*[ *]anyfs_ts_[A-Za-z0-9_]+\(' "$glue" \
                | grep -v '^static' \
                | grep -oE 'anyfs_ts_[A-Za-z0-9_]+'
              grep -hE '^DEF_P_TRAMP\(' "$glue" \
                | sed -E 's/^DEF_P_TRAMP\([[:space:]]*([A-Za-z0-9_]+).*/anyfs_ts_\1_p/'
            } | sort -u)"
    if [[ -z "$syms" ]]; then
        echo "wasm_exports: no anyfs_ts_* definitions found in $glue" >&2
        return 1
    fi
    out="_main,_malloc,_free"
    for s in $syms; do out+=",_$s"; done
    printf '%s\n' "$out"
}
