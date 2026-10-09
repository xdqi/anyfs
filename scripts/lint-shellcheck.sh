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
scripts/build_qemu_wasm.sh
scripts/build_anyfs.sh
scripts/build_anyfs_wasm.sh
scripts/build_wasm_sysroot.sh
scripts/lib/sysroot_sources.sh
scripts/build_linux_sysroot.sh
scripts/package_linux.sh
scripts/fetch_wasm_sysroot.sh
scripts/fetch_wasm_ld.sh
scripts/fetch_zig.sh
scripts/check_linux_abi.sh
scripts/lib/zig-cc.sh
scripts/lib/zig-cc
scripts/lib/zig-c++
scripts/lib/lkl-linux-cc.sh
scripts/sync_wasm_bundle.sh
scripts/lib/wasm_exports.sh
scripts/doctor.sh
scripts/lint-no-hardcoded-paths.sh
scripts/lint-shellcheck.sh
ts/packages/core/test/make-single-image.sh
ts/examples/electron-demo/scripts/collect-native.sh
ts/examples/electron-demo/scripts/collect-win64-dlls.sh
ts/examples/electron-demo/scripts/fetch-drivelist.sh
ts/examples/electron-demo/scripts/make-device-fixture.sh
ts/examples/electron-demo/scripts/make-smoke-fixture.sh
ts/examples/electron-demo/scripts/package.sh
ts/examples/electron-demo/scripts/sign-macos.sh
ts/examples/electron-demo/scripts/smoke-device.sh
ts/examples/electron-demo/scripts/smoke-package.sh
scripts/ci/run-restricted-win.sh
scripts/ci/test-device.sh
ts/examples/electron-demo/scripts/stage-native-win64.sh
ts/examples/electron-demo/scripts/verify-package.sh
scripts/build_macos_sysroot.sh
scripts/package_macos.sh
scripts/macho/zig-macos.sh
scripts/macho/macos-ar
scripts/macho/macos-ranlib
scripts/macho/check_macho.sh
scripts/macho/package_macos_tests.sh
scripts/macho/build_host_lib.sh
scripts/macho/build_smoke.sh
ts/packages/anyfs-native/scripts/build-macos.sh
ts/examples/electron-demo/scripts/stage-native-macos.sh
tests/macos/run-tests.sh
tests/macos/make-fixtures.sh
tests/device/run-cli-device-tests.sh
'
# shellcheck disable=SC2046,SC2086
# SC2046/SC2086: word-splitting on $checked and $(printf ...) is intentional —
# each whitespace-delimited path becomes a separate argument to shellcheck.
shellcheck -x -S warning $(printf "$root/%s " $checked)
