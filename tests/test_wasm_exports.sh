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

for must in _main _malloc _free _anyfs_ts_kernel_init _anyfs_ts_init_async \
            _anyfs_ts_session_open _anyfs_ts_session_enter_async \
            _anyfs_ts_session_enter_result_p _anyfs_ts_pread_p _anyfs_ts_close_p; do
    [[ ",$list," == *",$must,"* ]] || { echo "FAIL: $must missing from generated exports"; exit 1; }
done

n="$(tr ',' '\n' <<<"$list" | grep -c '^_anyfs_ts_')"
[[ "$n" -ge 30 ]] || { echo "FAIL: only $n anyfs_ts_* exports (expected >= 30)"; exit 1; }

# TS drift gate: every anyfs_ts_* string reference in the worker layer must be exported.
missing=0
while IFS= read -r sym; do
    [[ ",$list," == *",_$sym,"* ]] || { echo "FAIL: TS references $sym but it is not exported"; missing=1; }
done < <(grep -rhoE "'anyfs_ts_[a-z0-9_]+'" \
            "$root/ts/packages/core/src" | tr -d "'" | sort -u)
[[ "$missing" -eq 0 ]]

echo "OK: $n anyfs_ts_* exports, TS string references all covered"
