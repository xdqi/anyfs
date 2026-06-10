# shellcheck shell=bash
# scripts/lib/config.sh — source this to load build.config.toml (+ build.user.toml override)
# into ANYFS_* shell vars. Usage: source "$(dirname "$0")/lib/config.sh"
# Requires python3 (tomllib, 3.11+) — the build host already has it.
_anyfs_repo_root() { cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd; }

anyfs_load_config() {
    local root; root="$(_anyfs_repo_root)"
    eval "$(python3 - "$root" <<'PY'
import sys, tomllib, os, shlex
root = sys.argv[1]
def load(p):
    try:
        with open(p, "rb") as f: return tomllib.load(f)
    except FileNotFoundError: return {}
cfg = load(os.path.join(root, "build.config.toml"))
usr = load(os.path.join(root, "build.user.toml"))
def merge(a, b):
    for k, v in b.items():
        a[k] = merge(a.get(k, {}), v) if isinstance(v, dict) else v
    return a
cfg = merge(cfg, usr)
def shq(v): return shlex.quote(str(v))
def emit(prefix, d):
    for k, v in d.items():
        if isinstance(v, dict): emit(f"{prefix}{k.upper()}_", v)
        else: print(f'export ANYFS_{prefix}{k.upper()}={shq(v)}')
emit("", cfg)
PY
)"
    local deps="${ANYFS_PATHS_DEPS_ROOT:-deps}"
    local pfx="$root/"; [[ "$deps" = /* ]] && pfx=""
    : "${ANYFS_PATHS_LINUX_SRC:=$pfx$deps/linux}"
    : "${ANYFS_PATHS_QEMU_SRC:=$pfx$deps/qemu}"
    : "${ANYFS_PATHS_UTIL_LINUX:=$pfx$deps/util-linux}"
    : "${ANYFS_PATHS_KSMBD_TOOLS:=$pfx$deps/ksmbd-tools}"
    # Prefer a fetched sysroot (scripts/fetch_wasm_sysroot.sh) when nothing
    # was configured; an explicit build.user.toml paths.wasm_sysroot arrives
    # non-empty above and still wins.
    if [[ -z "${ANYFS_PATHS_WASM_SYSROOT:-}" && -d "$root/.toolchain/wasm-sysroot/lib" ]]; then
        ANYFS_PATHS_WASM_SYSROOT="$root/.toolchain/wasm-sysroot"
    fi
    : "${ANYFS_PATHS_WASM_SYSROOT:=$root/wasm-sysroot}"
    # Prefer a fetched wasm-ld (scripts/fetch_wasm_ld.sh) when nothing was
    # configured; an explicit build.user.toml toolchains.wasm_ld arrives
    # non-empty above and still wins.
    if [[ -z "${ANYFS_TOOLCHAINS_WASM_LD:-}" && -x "$root/.toolchain/wasm-ld/wasm-ld" ]]; then
        ANYFS_TOOLCHAINS_WASM_LD="$root/.toolchain/wasm-ld/wasm-ld"
    fi
    : "${ANYFS_TOOLCHAINS_WASM_LD:=$pfx$deps/llvm-wasm/workspace/install/llvm/bin/wasm-ld}"
    : "${ANYFS_TOOLCHAINS_EMSDK:=${EMSDK:-}}"
    export ANYFS_PATHS_LINUX_SRC ANYFS_PATHS_QEMU_SRC ANYFS_PATHS_UTIL_LINUX \
           ANYFS_PATHS_KSMBD_TOOLS ANYFS_PATHS_WASM_SYSROOT \
           ANYFS_TOOLCHAINS_WASM_LD ANYFS_TOOLCHAINS_EMSDK
}
anyfs_load_config
