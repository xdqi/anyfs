#!/bin/bash
# Assemble the macOS runtime test bundle, to be copied to a Mac (or a macOS CI
# runner) and run there with tests/run-tests.sh (docs/macos.md).
#
# Usage: package_macos_tests.sh --arch=arm64|x86_64 [--out=DIR]
#                               [--build-dir=DIR] [--native-dir=DIR]
#                               [--ubuntu=QCOW2] [--app=APP.app]
#
#   --build-dir   where the tools and core test programs are, laid out as
#                 build-anyfs-macos-<arch>/ (default: that directory); a flat
#                 directory with the same file names works too (a CI artifact)
#   --native-dir  anyfs_native.node + liblkl-kernel.dylib (default:
#                 ts/packages/anyfs-native/build-macos-<arch>/)
#   --ubuntu  a qcow2 cloud image (e.g. Ubuntu 26.10's) to include as
#             fixtures/ubuntu-26.10.qcow2; its reference data is computed
#             here, so this needs the linux-amd64 build of anyfs-lspart and
#             the Linux addon. Without it those tests are skipped.
#   --app     an electron-demo .app for ARCH with the addon staged
#             (ts/examples/electron-demo/scripts/stage-native-macos.sh);
#             included as app/<name>.app, and run-tests.sh then also tests
#             the addon from inside the packaged app
#   --out     default build/macos; writes <out>/anyfs-macos-test-<arch>/ and
#             <out>/anyfs-macos-test-<arch>.tar.gz
#
# Inputs: the macOS build (build_anyfs.sh --targets=macos-<arch>, all ninja
# targets) and the addon (ts/packages/anyfs-native/scripts/build-macos.sh).
# The fixtures come from tests/macos/make-fixtures.sh, which makes them byte
# for byte the same every time, so their reference data (lspart tables,
# partition metadata, file sizes and SHA-256, computed by the Linux build) is
# committed in tests/macos/reference/ and the Mac must reproduce it. When the
# Linux build is present, the reference is re-checked against it first.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
arch="" out="$REPO/build/macos" ubuntu="" app="" build="" native=""
for a in "$@"; do
    case "$a" in
        --arch=*)   arch="${a#--arch=}" ;;
        --out=*)    out="${a#--out=}" ;;
        --ubuntu=*) ubuntu="${a#--ubuntu=}" ;;
        --app=*)    app="${a#--app=}" ;;
        --build-dir=*)  build="${a#--build-dir=}" ;;
        --native-dir=*) native="${a#--native-dir=}" ;;
        -h|--help)  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 1 ;;
    esac
done
case "$arch" in
    arm64|x86_64) ;;
    *) echo "--arch=arm64|x86_64 is required" >&2; exit 1 ;;
esac
die() { echo "package_macos_tests: $*" >&2; exit 1; }

build="${build:-$REPO/build-anyfs-macos-$arch}"
native="${native:-$REPO/ts/packages/anyfs-native/build-macos-$arch}"
ref="$REPO/tests/macos/reference"
lin_lspart="$REPO/build-anyfs-linux-amd64/src/lspart/anyfs-lspart"
lin_addon="$REPO/ts/packages/anyfs-native/build/Release/anyfs_native.node"
TOOLS=(anyfs-ksmbd anyfs-nfsd anyfs-lspart)
TOOL_SRCS=(anyfs-ksmbd anyfs-nfsd src/lspart/anyfs-lspart)
# A flat directory (CI artifact) has anyfs-lspart at the top.
[[ -f "$build/src/lspart/anyfs-lspart" ]] || TOOL_SRCS=(anyfs-ksmbd anyfs-nfsd anyfs-lspart)
TESTS=(test_u8 test_path_dsl test_mount_opts test_name_escape test_legacy
       test_share_helpers test_tls_ca test_session_reopen test_session_whole_part
       test_qemu_thread)
for f in "${TOOL_SRCS[@]/#/$build/}" "${TESTS[@]/#/$build/}" "$native/anyfs_native.node" \
         "$native/liblkl-kernel.dylib"; do
    [[ -f "$f" ]] || die "missing $f"
done
[[ -z "$ubuntu" || -f "$ubuntu" ]] || die "--ubuntu: $ubuntu not found"
[[ -z "$ubuntu" || ( -f "$lin_lspart" && -f "$lin_addon" ) ]] \
    || die "--ubuntu needs the Linux build for its reference data ($lin_lspart, $lin_addon)"
[[ -z "$app" || -d "$app/Contents/Resources/native" ]] || die "--app: $app has no staged Resources/native"

dir="$out/anyfs-macos-test-$arch"
rm -rf "$dir" "$dir.tar.gz"
mkdir -p "$dir"/{bin,lib,native,fixtures,tests/lspart}
echo "$arch" > "$dir/ARCH"
for t in "${TOOL_SRCS[@]}" "${TESTS[@]}"; do cp "$build/$t" "$dir/bin/"; done
# anyfs-fuse when it was built (needs macFUSE on the Mac; run-tests.sh skips it otherwise).
[[ ! -f "$build/anyfs-fuse" ]] || cp "$build/anyfs-fuse" "$dir/bin/"
cp "$native/liblkl-kernel.dylib" "$dir/lib/"
cp "$native/anyfs_native.node" "$native/liblkl-kernel.dylib" "$dir/native/"
"$REPO/scripts/macho/check_macho.sh" --arch="$arch" --dylib=@rpath/liblkl-kernel.dylib \
    --rpath=@loader_path --rpath=@loader_path/../lib "${TOOLS[@]/#/$dir/bin/}" "${TESTS[@]/#/$dir/bin/}"
[[ ! -f "$dir/bin/anyfs-fuse" ]] || "$REPO/scripts/macho/check_macho.sh" --arch="$arch" \
    --dylib=@rpath/liblkl-kernel.dylib --dylib=/usr/local/lib/libfuse3.4.dylib \
    --rpath=@loader_path --rpath=@loader_path/../lib "$dir/bin/anyfs-fuse"

fx="$dir/fixtures"
t="$dir/tests"
bash "$REPO/tests/macos/make-fixtures.sh" "$fx" > /dev/null
cp "$REPO/tests/macos/run-tests.sh" "$REPO/tests/macos/native-smoke.mjs" "$REPO/tests/macos/README.md" "$t/"
cp "$ref/expected.json" "$t/expected.json"
cp "$ref"/lspart/*.txt "$t/lspart/"

# The committed reference must still be what the Linux build reports.
if [[ -f "$lin_lspart" && -f "$lin_addon" ]]; then
    for f in parts.img parts-zlib.dmg parts-bz2.dmg; do
        "$lin_lspart" "$fx/$f" 2>/dev/null | diff -u "$ref/lspart/$f.txt" - \
            || die "tests/macos/reference/lspart/$f.txt differs from the Linux build"
    done
    node "$REPO/tests/macos/native-smoke.mjs" --addon "$lin_addon" --fixtures "$fx" \
        --expected "$ref/expected.json" --loops 1 > "$out/reference-check-$arch.log" 2>&1 \
        || die "the Linux addon disagrees with tests/macos/reference/expected.json ($out/reference-check-$arch.log)"
    echo "package_macos_tests: reference re-checked against the Linux build"
else
    echo "package_macos_tests: no Linux build here; using the committed reference as is"
fi

if [[ -n "$ubuntu" ]]; then
    cp "$ubuntu" "$fx/ubuntu-26.10.qcow2"
    "$lin_lspart" "$fx/ubuntu-26.10.qcow2" > "$t/lspart/ubuntu-26.10.qcow2.txt" 2>/dev/null \
        || die "Linux anyfs-lspart failed on the Ubuntu image"
    node "$REPO/tests/macos/native-smoke.mjs" --addon "$lin_addon" --fixtures "$fx" \
        --write-expected "$t/ubuntu.json" "$REPO/tests/macos/ubuntu.spec.json" > "$out/reference-$arch.log" 2>&1 \
        || die "the Linux addon failed on the Ubuntu image ($out/reference-$arch.log)"
    python3 - "$t/expected.json" "$t/ubuntu.json" <<'PY'
import json, sys
base = json.load(open(sys.argv[1]))
base["images"] += json.load(open(sys.argv[2]))["images"]
json.dump(base, open(sys.argv[1], "w"), indent=2)
PY
    rm -f "$t/ubuntu.json"
fi
# One line per file for the shell tests: <fixture> <partition> <path> <size> <sha256>
python3 - "$t/expected.json" > "$t/expected-files.txt" <<'PY'
import json, sys
for img in json.load(open(sys.argv[1]))["images"]:
    for f in img["files"]:
        print(img["file"], img["enter"], f["path"], f["size"], f["sha256"])
PY

if [[ -n "$app" ]]; then
    mkdir -p "$dir/app"
    cp -a "$app" "$dir/app/"
fi
(cd "$dir" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar czf "$dir.tar.gz" -C "$out" "$(basename "$dir")"
echo "package_macos_tests: $dir.tar.gz ($(du -h "$dir.tar.gz" | cut -f1))"
