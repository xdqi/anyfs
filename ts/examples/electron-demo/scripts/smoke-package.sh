#!/usr/bin/env bash
# Run the headless smokes of a packaged app: the native smoke
# (src/native-smoke.ts), checked against the fixture (make-smoke-fixture.sh
# output), and the drives smoke (ANYFS_DRIVES_SMOKE), which lists the host's
# disks through the staged drivelist addon.
# Works on Linux (wrap in xvfb-run when there is no display), macOS, and
# Windows under Git Bash.
#
# Usage: smoke-package.sh <packaged-dir> <linux|win32|darwin> <fixture-dir> [report.json]
set -euo pipefail

pkg="${1:?usage: smoke-package.sh <packaged-dir> <platform> <fixture-dir> [report.json]}"
platform="${2:?platform}"
fixture="${3:?fixture dir}"
report="${4:-$fixture/smoke-report-$platform.json}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
# Normalize (Git Bash on Windows hands us D:\a\_temp style paths).
pkg="$(cd "$pkg" && pwd)"
fixture="$(cd "$fixture" && pwd)"
report="$(cd "$(dirname "$report")" && pwd)/$(basename "$report")"

case "$platform" in
linux) exe="$pkg/anyfs-demo"; args=(--no-sandbox) ;;
win32) exe="$pkg/anyfs-demo.exe"; args=() ;;
darwin) exe="$pkg/anyfs-demo.app/Contents/MacOS/anyfs-demo"; args=() ;;
*) echo "smoke-package: unknown platform $platform" >&2; exit 2 ;;
esac

native_path() {
    # Windows binaries want C:\... paths, not Git Bash's /c/...
    if [[ "$platform" == win32 ]] && command -v cygpath >/dev/null; then cygpath -w "$1"; else echo "$1"; fi
}

image="$(native_path "$fixture/smoke.qcow2")"
out="$(native_path "$report")"
field() {
    node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"))[process.argv[2]])' \
        "$(native_path "$fixture/expected.json")" "$1"
}
part="$(field part)"
read_file="$(field read)"
rm -f "$report"

# ELECTRON_RUN_AS_NODE would turn the app into a bare Node runtime.
start=$SECONDS
set +e
env -u ELECTRON_RUN_AS_NODE \
    ANYFS_NATIVE_SMOKE=1 ANYFS_NATIVE_IMAGE="$image" ANYFS_NATIVE_PART="$part" \
    ANYFS_NATIVE_READ="$read_file" ANYFS_NATIVE_OUT="$out" \
    "$exe" ${args[@]+"${args[@]}"}
rc=$?
set -e
echo "smoke-package: app exited rc=$rc after $((SECONDS - start))s"
[[ -f "$report" ]] && cat "$report"
[[ $rc -eq 0 ]] || { echo "smoke-package: app exit code $rc" >&2; exit 1; }
[[ -f "$report" ]] || { echo "smoke-package: no report written" >&2; exit 1; }
node "$script_dir/check-smoke.mjs" "$out" "$(native_path "$fixture/expected.json")"

drives_report="${report%.json}-drives.json"
rm -f "$drives_report"
set +e
env -u ELECTRON_RUN_AS_NODE ANYFS_DRIVES_SMOKE=1 ANYFS_DRIVES_OUT="$(native_path "$drives_report")" \
    "$exe" ${args[@]+"${args[@]}"}
rc=$?
set -e
echo "smoke-package: drives smoke exited rc=$rc"
[[ -f "$drives_report" ]] || { echo "smoke-package: no drives report written" >&2; exit 1; }
node "$script_dir/check-drives.mjs" "$(native_path "$drives_report")"
[[ $rc -eq 0 ]] || { echo "smoke-package: drives smoke exit code $rc" >&2; exit 1; }
