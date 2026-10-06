# Robustness gate: findings

Results of `ts/tests/robustness/run.mjs` that are not a plain `ok` / `error` / `fatal`:
native hangs and crashes (recorded, not gated), and anything the wasm gate caught and was fixed.

Reproduce one case with `node ts/tests/robustness/run.mjs --backend <backend> --only <case>`. Its
kernel log is in `~/.cache/anyfs-robustness/logs/<backend>/<case>.log`.

Status: OPEN · FIXED (commit)

## Native backend (non-gating)

| case | class | last step | reason | status |
| ---- | ----- | --------- | ------ | ------ |

## wasm gate

| case | class | last step | root cause | status |
| ---- | ----- | --------- | ---------- | ------ |
