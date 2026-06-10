#!/usr/bin/env bash
# Lint gate: runs shellcheck (severity >= warning) on build scripts. Extend
# the list below as scripts are cleaned; new scripts must be added here.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
checked='
scripts/lib/config.sh
scripts/gen_lkl_config.sh
scripts/build_lkl.sh
scripts/gen_lkl_config_wasm.sh
scripts/build_lkl_wasm.sh
scripts/build_boot_wasm.sh
scripts/build_libblkid_wasm.sh
scripts/build_libblkid_mingw.sh
scripts/build_qemu.sh
scripts/build_anyfs.sh
scripts/lint-no-hardcoded-paths.sh
scripts/lint-shellcheck.sh
'
# shellcheck disable=SC2046,SC2086
# SC2046/SC2086: word-splitting on $checked and $(printf ...) is intentional —
# each whitespace-delimited path becomes a separate argument to shellcheck.
shellcheck -x -S warning $(printf "$root/%s " $checked)
