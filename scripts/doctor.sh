#!/usr/bin/env bash
# scripts/doctor.sh — validate the toolchain/deps before a build. Exit non-zero on any failure.
set -uo pipefail
_root="$(cd "$(dirname "$0")/.." && pwd)"
# Capture env overrides before config.sh (which exports toml defaults, overwriting env vars).
_pre_wasm_ld="${ANYFS_TOOLCHAINS_WASM_LD:-}"
source "$(dirname "$0")/lib/config.sh"
# Restore env override if caller set it explicitly before running doctor.
[ -n "$_pre_wasm_ld" ] && ANYFS_TOOLCHAINS_WASM_LD="$_pre_wasm_ld"
fail=0
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=1; }

echo "== tools =="
command -v meson    >/dev/null && ok "meson"            || bad "meson not found"
command -v ninja    >/dev/null && ok "ninja"            || bad "ninja not found"
command -v peru     >/dev/null && ok "peru"             || bad "peru not found (pip install peru)"
command -v python3  >/dev/null && ok "python3"          || bad "python3 not found"

echo "== native binutils (>= 2.30 for kernel 6.13+) =="
v=$("$ANYFS_TOOLCHAINS_BINUTILS_NATIVE/ld" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
if [ -n "$v" ] && awk -v v="$v" 'BEGIN{split(v,a,".");exit !(a[1]+0>2||(a[1]+0==2&&a[2]+0>=30))}'; then
    ok "ld $v"
else
    bad "native ld < 2.30 or missing ($ANYFS_TOOLCHAINS_BINUTILS_NATIVE/ld)"
fi

echo "== mingw cross (msys2-cross, carries binutils 2.46 + PE weak-symbol patch) =="
mw="$ANYFS_TOOLCHAINS_MSYS2_CROSS/bin/x86_64-w64-mingw32-ld"
[ -x "$mw" ] && ok "$($mw --version | head -1)" || bad "mingw64 ld missing: $mw"

echo "== wasm-ld (patched, from xdqi/llvm-wasm — only this binary is consumed) =="
wl="${ANYFS_TOOLCHAINS_WASM_LD:-}"
if [ -z "$wl" ]; then
    case "$ANYFS_PATHS_DEPS_ROOT" in
        /*) wl="$ANYFS_PATHS_DEPS_ROOT/llvm-wasm/workspace/install/llvm/bin/wasm-ld" ;;
        *)  wl="$_root/$ANYFS_PATHS_DEPS_ROOT/llvm-wasm/workspace/install/llvm/bin/wasm-ld" ;;
    esac
fi
if [ -x "$wl" ]; then
    ver=$("$wl" --version 2>/dev/null | grep -oE 'LLD [0-9]+' | head -1)
    [ "$ver" = "LLD 18" ] && ok "$ver ($wl)" || bad "wasm-ld is '$ver', expected LLD 18 ($wl)"
else
    bad "patched wasm-ld not built — run: (cd deps/llvm-wasm && ./linux-wasm.sh build-llvm)"
fi

echo "== deps synced + at locked SHA =="
if [ -d "$ANYFS_PATHS_LINUX_SRC/.git" ]; then
    ok "linux at $(git -C "$ANYFS_PATHS_LINUX_SRC" rev-parse --short HEAD)"
else
    bad "deps not synced — run: peru sync"
fi

[ "$fail" -eq 0 ] && echo "doctor: all checks passed" || echo "doctor: FAILURES above"
exit "$fail"
