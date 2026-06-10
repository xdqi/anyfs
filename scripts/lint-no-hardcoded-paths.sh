#!/usr/bin/env bash
# Fail if build scripts still hardcode $HOME or /opt paths (use build.config.toml instead).
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# Scripts that have been migrated to config (extend this allowlist as P1 progresses).
migrated='scripts/gen_lkl_config.sh scripts/build_lkl.sh scripts/gen_lkl_config_wasm.sh scripts/build_lkl_wasm.sh scripts/build_boot_wasm.sh scripts/build_libblkid_wasm.sh scripts/build_libblkid_mingw.sh scripts/build_qemu.sh scripts/build_anyfs.sh scripts/build_anyfs_wasm.sh scripts/build_wasm_sysroot.sh scripts/fetch_wasm_sysroot.sh'
rc=0
for f in $migrated; do
    if grep -nE '\$HOME|/opt/msys2|/home/[a-z]+/' "$root/$f"; then
        echo "FAIL: hardcoded path in $f (use scripts/lib/config.sh)"; rc=1
    fi
done
[ "$rc" -eq 0 ] && echo "lint: no hardcoded paths in migrated scripts"
exit "$rc"
