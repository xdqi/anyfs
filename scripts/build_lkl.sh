#!/bin/bash
# Build LKL (tools/lkl) for one or more pre-configured out-of-tree build dirs.
#
# Usage: ./build_lkl.sh [OPTIONS]
#
# Options:
#   --linux=DIR         Kernel source tree (default: from build.config.toml; linux_src or deps/linux)
#   --out=DIR           Parent dir containing lkl-<target>/ build trees
#                       (default: repo root)
#   --targets=LIST      Comma-separated subset of:
#                         linux-amd64,linux-arm64,mingw32,mingw64
#                       (default: linux-amd64,mingw32,mingw64)
#   --clean             Run `make clean` in each target before building
#   --cc=CMD            C compiler override passed to make as CC= (e.g. "sccache gcc")
#   --sccache           Compile through sccache (and its dist farm), wired the
#                       way each target needs; see sccache_cc_for. Overrides --cc.
#   -j N                Parallelism (default: nproc)
#
# Expects each lkl-<target>/ to already contain a .config and (for mingw
# targets) tools/lkl/Makefile.conf + include/lkl_autoconf.h. Generate them
# with the companion script: gen_lkl_config.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

# LINUX_DIR / OUT_PARENT: CLI --linux= / --out= win; config.sh provides defaults.
LINUX_DIR="${LINUX_DIR:-$ANYFS_PATHS_LINUX_SRC}"
OUT_PARENT="${OUT_PARENT:-$(cd "$(dirname "$0")/.." && pwd)}"
TARGETS_REQ=""
DO_CLEAN=0
JOBS="$(nproc)"
CC_OVERRIDE=""
USE_SCCACHE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux=*)   LINUX_DIR="${1#--linux=}"; shift ;;
        --linux)     LINUX_DIR="$2"; shift 2 ;;
        --out=*)     OUT_PARENT="${1#--out=}"; shift ;;
        --out)       OUT_PARENT="$2"; shift 2 ;;
        --targets=*) TARGETS_REQ="${1#--targets=}"; shift ;;
        --targets)   TARGETS_REQ="$2"; shift 2 ;;
        --clean)     DO_CLEAN=1; shift ;;
        --cc=*)      CC_OVERRIDE="${1#--cc=}"; shift ;;
        --cc)        CC_OVERRIDE="$2"; shift 2 ;;
        --sccache)   USE_SCCACHE=1; shift ;;
        -j)          JOBS="$2"; shift 2 ;;
        -j*)         JOBS="${1#-j}"; shift ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

TARGETS_REQ="${TARGETS_REQ:-linux-amd64,mingw32,mingw64}"

if [[ ! -d "$LINUX_DIR/tools/lkl" ]]; then
    echo "Error: $LINUX_DIR/tools/lkl not found. Is --linux=$LINUX_DIR correct?" >&2
    exit 1
fi

cross_for() {
    case "$1" in
        linux-amd64) echo "" ;;
        linux-arm64) echo "aarch64-linux-gnu-" ;;
        mingw32)     echo "i686-w64-mingw32-" ;;
        mingw64)     echo "x86_64-w64-mingw32-" ;;
        *) echo "Unknown target: $1" >&2; return 1 ;;
    esac
}

# CC for building target $1 (cross prefix $2) through sccache. The compiler
# goes to sccache by absolute path, so sccache hashes and ships the real
# toolchain. mingw64 has two compilers (the tools/lkl/bin shim sends kernel
# code to cygwin-gcc), so it goes through lib/lkl-mingw-cc.sh; mingw32 has
# no shim (ILP32 long is pointer-sized) and uses its gcc directly.
sccache_cc_for() {
    local name="$1" cross="$2" cc
    if [[ "$name" == mingw64 ]]; then
        cc="$(command -v x86_64-pc-cygwin-gcc)" \
            || { echo "Error: x86_64-pc-cygwin-gcc not on PATH" >&2; return 1; }
        echo "$SCRIPT_DIR/lib/lkl-mingw-cc.sh $cc ${cross}gcc"
        return
    fi
    cc="$(command -v "${cross}gcc")" \
        || { echo "Error: ${cross}gcc not on PATH" >&2; return 1; }
    echo "sccache $cc"
}

build_one() {
    local NAME="$1"
    local CROSS="$2"
    local OUT="$OUT_PARENT/lkl-$NAME"

    if [[ ! -f "$OUT/.config" ]]; then
        echo "Error: $OUT/.config not found. Run gen_lkl_config.sh first." >&2
        return 1
    fi

    echo
    echo "=============================================================="
    echo "  Building lkl-$NAME (CROSS=${CROSS:-<native>})"
    echo "  OUT: $OUT"
    echo "=============================================================="

    local cross_arg=()
    [[ -n "$CROSS" ]] && cross_arg=(CROSS_COMPILE="$CROSS")

    local cc_arg=()
    [[ -n "$CC_OVERRIDE" ]] && cc_arg=(CC="$CC_OVERRIDE")
    if [[ $USE_SCCACHE -eq 1 ]]; then
        local scc
        scc="$(sccache_cc_for "$NAME" "$CROSS")" || return 1
        cc_arg=(CC="$scc")
        echo "  CC: $scc"
    fi

    # OUTPUT must go through the environment, not as a make CLI arg — the
    # tools/lkl Makefile rewrites OUTPUT to "$OUTPUT/tools/lkl/", and a CLI
    # assignment would defeat that rewrite (GNU make precedence).
    if [[ $DO_CLEAN -eq 1 ]]; then
        OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" \
             ARCH=lkl "${cross_arg[@]}" "${cc_arg[@]}" clean || true
    fi

    OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" -j"$JOBS" \
         ARCH=lkl "${cross_arg[@]}" "${cc_arg[@]}"

    echo
    echo "Output for lkl-$NAME:"
    ls -lh "$OUT/tools/lkl/liblkl.a" \
           "$OUT/tools/lkl/lib/liblkl."* 2>/dev/null || true
}

IFS=',' read -ra TARGETS_ARR <<< "$TARGETS_REQ"

# Validate target names up front
for T in "${TARGETS_ARR[@]}"; do
    cross_for "$T" >/dev/null
done

FAILED=()
for T in "${TARGETS_ARR[@]}"; do
    if ! build_one "$T" "$(cross_for "$T")"; then
        FAILED+=("$T")
    fi
done

echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo "=== Build complete for: ${TARGETS_ARR[*]} ==="
else
    echo "=== Build FAILED for: ${FAILED[*]} ==="
    echo "=== Succeeded:        $(comm -23 <(printf '%s\n' "${TARGETS_ARR[@]}" | sort) <(printf '%s\n' "${FAILED[@]}" | sort) | tr '\n' ' ')==="
    exit 1
fi
