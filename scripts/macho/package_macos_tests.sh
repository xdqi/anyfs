#!/bin/bash
# Assemble the macOS runtime test bundle, to be copied to a Mac and run there
# (tests/macos/run-tests.sh, docs/macos.md).
#
# Usage: package_macos_tests.sh --arch=arm64|x86_64 [--out=DIR]
#                               [--ubuntu=QCOW2] [--app=APP.app]
#
#   --ubuntu  a qcow2 cloud image (e.g. Ubuntu 26.10's) to include as
#             fixtures/ubuntu-26.10.qcow2; without it those tests are skipped
#   --app     an electron-demo .app for ARCH with the addon staged
#             (ts/examples/electron-demo/scripts/stage-native-macos.sh);
#             included as app/<name>.app, and run-tests.sh then also tests
#             the addon from inside the packaged app
#   --out     default build/macos; writes <out>/anyfs-macos-test-<arch>/ and
#             <out>/anyfs-macos-test-<arch>.tar.gz
#
# Inputs: the macOS build (build_anyfs.sh --targets=macos-<arch>, all ninja
# targets), the addon (ts/packages/anyfs-native/scripts/build-macos.sh), and
# the linux-amd64 build of anyfs-lspart plus the Linux addon: the reference
# data (lspart tables, partition metadata, file sizes and hashes) is computed
# on Linux from the exact fixture bytes shipped, and the Mac must reproduce it.
# The fixture parts.img is made by ts/packages/core/test/make-parts-image.sh
# (GPT: BIOS boot, vfat ESP, ext4 metadata_csum "fixroot"); the two DMGs wrap
# it (tests/make_dmg_image.py, zlib and bzip2).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
arch="" out="$REPO/build/macos" ubuntu="" app=""
for a in "$@"; do
    case "$a" in
        --arch=*)   arch="${a#--arch=}" ;;
        --out=*)    out="${a#--out=}" ;;
        --ubuntu=*) ubuntu="${a#--ubuntu=}" ;;
        --app=*)    app="${a#--app=}" ;;
        -h|--help)  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 1 ;;
    esac
done
case "$arch" in
    arm64|x86_64) ;;
    *) echo "--arch=arm64|x86_64 is required" >&2; exit 1 ;;
esac
die() { echo "package_macos_tests: $*" >&2; exit 1; }

build="$REPO/build-anyfs-macos-$arch"
native="$REPO/ts/packages/anyfs-native/build-macos-$arch"
lin_lspart="$REPO/build-anyfs-linux-amd64/src/lspart/anyfs-lspart"
lin_addon="$REPO/ts/packages/anyfs-native/build/Release/anyfs_native.node"
TOOLS=(anyfs-ksmbd anyfs-nfsd anyfs-lspart)
TOOL_SRCS=(anyfs-ksmbd anyfs-nfsd src/lspart/anyfs-lspart)
TESTS=(test_u8 test_path_dsl test_mount_opts test_name_escape test_legacy
       test_share_helpers test_tls_ca test_session_reopen test_session_whole_part
       test_qemu_thread)
for f in "${TOOL_SRCS[@]/#/$build/}" "${TESTS[@]/#/$build/}" "$native/anyfs_native.node" \
         "$native/liblkl-kernel.dylib" "$lin_lspart" "$lin_addon"; do
    [[ -f "$f" ]] || die "missing $f"
done
[[ -z "$ubuntu" || -f "$ubuntu" ]] || die "--ubuntu: $ubuntu not found"
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

# Fixtures.
fx="$dir/fixtures"
bash "$REPO/ts/packages/core/test/make-parts-image.sh" "$fx/parts.img" > /dev/null
python3 "$REPO/tests/make_dmg_image.py" --codec zlib "$fx/parts.img" "$fx/parts-zlib.dmg"
python3 "$REPO/tests/make_dmg_image.py" --codec bz2 "$fx/parts.img" "$fx/parts-bz2.dmg"
[[ -z "$ubuntu" ]] || cp "$ubuntu" "$fx/ubuntu-26.10.qcow2"

# Reference data from the Linux build, on these exact bytes.
t="$dir/tests"
cp "$REPO/tests/macos/run-tests.sh" "$REPO/tests/macos/native-smoke.mjs" "$REPO/tests/macos/README.md" "$t/"
for f in "$fx"/*; do
    "$lin_lspart" "$f" > "$t/lspart/$(basename "$f").txt" 2>/dev/null \
        || die "Linux anyfs-lspart failed on $f"
done
node "$REPO/tests/macos/native-smoke.mjs" --addon "$lin_addon" --fixtures "$fx" \
    --write-expected "$t/expected.json" "$REPO/tests/macos/fixtures.spec.json" > "$out/reference-$arch.log" 2>&1 \
    || die "the Linux addon failed to produce the reference data ($out/reference-$arch.log)"
python3 - "$t/expected.json" > "$t/expected-files.txt" <<'PY'
import json, sys
for img in json.load(open(sys.argv[1]))["images"]:
    for f in img["files"]:
        if "sha256" in f:
            print(img["file"], img["enter"], f["path"], f["size"], f["sha256"])
PY

if [[ -n "$app" ]]; then
    mkdir -p "$dir/app"
    cp -a "$app" "$dir/app/"
fi
(cd "$dir" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar czf "$dir.tar.gz" -C "$out" "$(basename "$dir")"
echo "package_macos_tests: $dir.tar.gz ($(du -h "$dir.tar.gz" | cut -f1))"
