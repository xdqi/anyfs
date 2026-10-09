#!/usr/bin/env bash
# Check a packaged app's contents before it is archived: Electron binary, the
# bundled main process, the renderer with its hashed wasm fallback, and (unless
# --no-native) the native payload with the right binary format and a complete
# dependency closure. A package that is missing its native addon still starts
# and quietly runs on wasm, so this gate must fail instead.
#
# Usage: verify-package.sh <packaged-dir> <linux|win32|darwin> <x64|arm64> [--no-native]
set -euo pipefail

pkg="${1:?usage: verify-package.sh <packaged-dir> <platform> <arch> [--no-native]}"
platform="${2:?platform}"
arch="${3:?arch}"
no_native=0
[[ "${4:-}" == --no-native ]] && no_native=1

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../../../.." && pwd)"
fail=0
err() { echo "verify-package: $*" >&2; fail=1; }
need() { [[ -e "$1" ]] || err "missing ${1#"$pkg"/}"; }

case "$platform" in
linux)
    exe="$pkg/anyfs-demo"; res="$pkg/resources"
    node_fmt='ELF 64-bit LSB shared object'
    ;;
win32)
    exe="$pkg/anyfs-demo.exe"; res="$pkg/resources"
    node_fmt='PE32\+ executable.*DLL'
    ;;
darwin)
    exe="$pkg/anyfs-demo.app/Contents/MacOS/anyfs-demo"; res="$pkg/anyfs-demo.app/Contents/Resources"
    node_fmt='Mach-O 64-bit .*(bundle|dynamically linked shared library)'
    ;;
*) echo "verify-package: unknown platform $platform" >&2; exit 2 ;;
esac
case "$platform/$arch" in
linux/x64) arch_fmt='x86-64' ;;
linux/arm64) arch_fmt='aarch64' ;;
win32/x64) arch_fmt='x86-64' ;;
darwin/x64) arch_fmt='x86_64' ;;
darwin/arm64) arch_fmt='arm64' ;;
*) echo "verify-package: unsupported $platform/$arch" >&2; exit 2 ;;
esac

need "$exe"
for f in main.cjs preload.cjs http-proxy-worker.cjs; do need "$res/app/dist/$f"; done
need "$res/app/package.json"
need "$res/build-info.json"
need "$res/renderer/index.html"
need "$res/renderer/anyfs-worker.js"
[[ ! -e "$res/renderer/disks" ]] || err "renderer/disks (demo images) must not ship"

# The wasm fallback: exactly one /wasm/<16-hex hash>/ set (vite.config.ts).
wasm_dirs=()
while IFS= read -r d; do wasm_dirs+=("$d"); done < <(
    find "$res/renderer/wasm" -mindepth 1 -maxdepth 1 -type d 2>/dev/null |
        grep -E '/[0-9a-f]{16}$' || true)
if [[ ${#wasm_dirs[@]} -ne 1 ]]; then
    err "expected one renderer/wasm/<hash>/ dir, found ${#wasm_dirs[@]}"
else
    for f in anyfs.mjs anyfs.wasm anyfs.worker.js; do need "${wasm_dirs[0]}/$f"; done
fi

if [[ $no_native -eq 0 ]]; then
    addon="$res/native/anyfs_native.node"
    need "$addon"
    # Drive listing: the drivelist addon plus its JS, which esbuild inlines
    # into main.cjs only when drivelist was installed at build time.
    need "$res/native/drivelist.node"
    [[ "$platform" != darwin ]] || need "$res/native/liblkl-kernel.dylib"
    if grep -q 'require("drivelist")' "$res/app/dist/main.cjs"; then
        err "main.cjs does not bundle drivelist (it was not installed when build:main ran)"
    fi
    for f in "$res/native/"*.node; do
        [[ -f "$f" ]] || continue
        desc="$(file -b "$f")"
        echo "  native/$(basename "$f"): $desc"
        [[ "$desc" =~ $node_fmt ]] || err "$(basename "$f") is not a $platform addon: $desc"
        [[ "$desc" == *"$arch_fmt"* ]] || err "$(basename "$f") is not $arch: $desc"
    done
    case "$platform" in
    linux)
        # Electron 42 needs glibc 2.25; the addons may not ask for more and
        # may only leave napi_* undefined (resolved from the host binary).
        for f in "$res/native/"*.node; do
            "$repo_root/scripts/check_linux_abi.sh" --allow-undefined='^(napi_|node_api_)' \
                2.25 "$f" || err "$(basename "$f") fails the glibc 2.25 ABI gate"
        done
        ;;
    win32)
        # Every non-system DLL the addons import must ship next to them.
        closure="$(mktemp -d)"
        OBJDUMP="${OBJDUMP:-objdump}" bash "$script_dir/collect-win64-dlls.sh" "$closure" \
            "$res/native/"*.node -- "$res/native" >/dev/null ||
            err "native DLL closure incomplete"
        rm -rf "$closure"
        ;;
    esac
    # Linux links LKL and QEMU into the addon itself (tens of MiB); win32
    # imports them as DLLs, which the closure check above covers.
    if [[ "$platform" == linux && "$(du -sk "$addon" | cut -f1)" -lt 10240 ]]; then
        err "anyfs_native.node is too small to contain LKL"
    fi
else
    [[ ! -e "$res/native/anyfs_native.node" ]] || err "--no-native package contains anyfs_native.node"
fi

if [[ $fail -ne 0 ]]; then
    echo "verify-package: FAILED for $pkg" >&2
    exit 1
fi
echo "verify-package: OK ($platform/$arch$([[ $no_native -eq 1 ]] && echo ', wasm only'))"
