#!/usr/bin/env bash
# Headless native smoke of a packaged app against the read-only test device
# that scripts/ci/test-device.{sh,ps1} attached (virtual block device backed
# by make-device-fixture.sh's parts.img), not against an image file.
#
#   allow: the app must open the device, list its partitions, mount the
#          reference partition, and read the reference file with the
#          reference size and sha256 (check-smoke.mjs against expected.json).
#   deny:  the device node is not readable for this user (Linux/macOS:
#          test-device.sh perm deny; Windows: the app runs with a restricted
#          non-admin token); the app must fail sessionOpen with a permission
#          error and exit non-zero, not hang.
#
# Usage: smoke-device.sh <packaged-dir> <linux|win32|darwin> <state-dir> <fixture-dir>
#                        <allow|deny> [<report.json>]
# SMOKE_DEVICE overrides the node to open (e.g. macOS's ANYFS_TEST_RAW_DEVICE).
set -euo pipefail

pkg="${1:?usage}" platform="${2:?usage}" state="${3:?usage}" fixture="${4:?usage}" expect="${5:?usage}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
pkg="$(cd "$pkg" && pwd)"
fixture="$(cd "$fixture" && pwd)"
report="${6:-$fixture/device-report-$expect.json}"
report="$(cd "$(dirname "$report")" && pwd)/$(basename "$report")"
# shellcheck disable=SC1091
source "$state/device.env"
device="${SMOKE_DEVICE:-$ANYFS_TEST_DEVICE}"

case "$platform" in
linux) exe="$pkg/anyfs-demo"; args=(--no-sandbox) ;;
win32) exe="$pkg/anyfs-demo.exe"; args=() ;;
darwin) exe="$pkg/anyfs-demo.app/Contents/MacOS/anyfs-demo"; args=() ;;
*) echo "smoke-device: unknown platform $platform" >&2; exit 2 ;;
esac
native_path() {
    if [[ "$platform" == win32 ]] && command -v cygpath > /dev/null; then cygpath -w "$1"; else echo "$1"; fi
}
field() {
    node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"))[process.argv[2]])' \
        "$(native_path "$fixture/expected.json")" "$1"
}

out="$(native_path "$report")"
rm -f "$report" "$report.rc"
echo "smoke-device: $expect, device $device"
set +e
if [[ "$platform" == win32 && "$expect" == deny ]]; then
    # The CI account is an administrator, and a raw disk needs admin rights:
    # run the app with a restricted Basic User token (no Administrators
    # group) instead. runas returns at once; wait for the wrapper's rc file.
    wrapper="$(dirname "$report")/smoke-device-deny.cmd"
    {
        printf '@echo off\r\n'
        printf 'set ELECTRON_RUN_AS_NODE=\r\n'
        printf 'set ANYFS_NATIVE_SMOKE=1\r\n'
        printf 'set ANYFS_NATIVE_IMAGE=%s\r\n' "$device"
        printf 'set ANYFS_NATIVE_PART=%s\r\n' "$(field part)"
        printf 'set ANYFS_NATIVE_READ=%s\r\n' "$(field read)"
        printf 'set ANYFS_NATIVE_OUT=%s\r\n' "$out"
        printf '"%s"\r\n' "$(native_path "$exe")"
        printf 'echo %%ERRORLEVEL%% > "%s"\r\n' "$(native_path "$report.rc")"
    } > "$wrapper"
    MSYS_NO_PATHCONV=1 runas /trustlevel:0x20000 "cmd /c \"$(native_path "$wrapper")\""
    for _ in $(seq 1 180); do [[ -f "$report.rc" ]] && break; sleep 1; done
    [[ -f "$report.rc" ]] || { echo "smoke-device: restricted run did not finish" >&2; exit 1; }
    rc="$(tr -dc 0-9 < "$report.rc")"
else
    env -u ELECTRON_RUN_AS_NODE \
        ANYFS_NATIVE_SMOKE=1 ANYFS_NATIVE_IMAGE="$device" \
        ANYFS_NATIVE_PART="$(field part)" ANYFS_NATIVE_READ="$(field read)" ANYFS_NATIVE_OUT="$out" \
        "$exe" ${args[@]+"${args[@]}"}
    rc=$?
fi
set -e
echo "smoke-device: app exited rc=$rc"
[[ -f "$report" ]] || { echo "smoke-device: no report written" >&2; exit 1; }
cat "$report"

if [[ "$expect" == allow ]]; then
    [[ $rc -eq 0 ]] || { echo "smoke-device: app exit code $rc" >&2; exit 1; }
    node "$script_dir/check-smoke.mjs" "$out" "$(native_path "$fixture/expected.json")"
else
    [[ $rc -ne 0 ]] || { echo "smoke-device: the app opened a device it must not read" >&2; exit 1; }
    node -e '
const r = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
const re = /permission denied|access is denied|operation not permitted|EACCES|EPERM/i;
if (r.ok !== false || !re.test(r.error ?? "")) {
    console.error(`smoke-device: expected a permission error, got ok=${r.ok} error=${r.error}`);
    process.exit(1);
}
console.log(`smoke-device: denied as expected: ${r.error}`);' "$out"
fi
