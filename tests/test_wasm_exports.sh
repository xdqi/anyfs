#!/usr/bin/env bash
# Gate for the generated wasm export list:
#   1. the generator emits every known-core symbol,
#   2. every anyfs_ts_* string reference in the TS worker layer is exported
#      (catches TS<->C drift that previously bit build_anyfs_browser_wasm.sh).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../scripts/lib/wasm_exports.sh
source "$root/scripts/lib/wasm_exports.sh"

list="$(anyfs_wasm_exports "$root/ts/native/anyfs_ts.c")"

for must in _main _malloc _free _anyfs_ts_api_submit _anyfs_ts_kernel_init \
            _anyfs_ts_session_open _anyfs_ts_session_enter _anyfs_ts_pread \
            _anyfs_ts_close; do
    [[ ",$list," == *",$must,"* ]] || { echo "FAIL: $must missing from generated exports"; exit 1; }
done

n="$(tr ',' '\n' <<<"$list" | grep -c '^_anyfs_ts_')"
[[ "$n" -ge 15 ]] || { echo "FAIL: only $n anyfs_ts_* exports (expected >= 15)"; exit 1; }

# A parameter type must never be mistaken for an exported function.
if tr ',' '\n' <<<"$list" | grep -qx '_anyfs_ts_req'; then
    echo "FAIL: struct anyfs_ts_req picked up as an export"
    exit 1
fi

# API-thread op numbers: ANYFS_TS_OP_* in the C glue must match ApiOp in
# wasm-api.ts exactly (the JS side writes these numbers into the request).
c_ops="$(grep -oE 'ANYFS_TS_OP_[A-Z_]+ = [0-9]+' "$root/ts/native/anyfs_ts.c" \
         | sed -E 's/ANYFS_TS_OP_([A-Z_]+) = ([0-9]+)/\1=\2/' | sort)"
ts_ops="$(awk '/^export const ApiOp = \{/{f=1; next} f && /^\} as const/{f=0} f' \
              "$root/ts/packages/core/src/wasm-api.ts" \
          | grep -oE '^[[:space:]]+[A-Z_]+: [0-9]+' | tr -d ' ' | tr ':' '=' | sort)"
[[ -n "$c_ops" && "$c_ops" == "$ts_ops" ]] || {
    echo "FAIL: ANYFS_TS_OP_* (anyfs_ts.c) and ApiOp (wasm-api.ts) differ"
    diff <(echo "$c_ops") <(echo "$ts_ops") || true
    exit 1
}

# TS drift gate: every anyfs_ts_* string reference in the worker layer must be exported.
missing=0
while IFS= read -r sym; do
    [[ ",$list," == *",_$sym,"* ]] || { echo "FAIL: TS references $sym but it is not exported"; missing=1; }
done < <(grep -rhoE "'anyfs_ts_[a-z0-9_]+'" \
            "$root/ts/packages/core/src" | tr -d "'" | sort -u)
[[ "$missing" -eq 0 ]]

echo "OK: $n anyfs_ts_* exports, TS string references covered, API op numbers in sync"
