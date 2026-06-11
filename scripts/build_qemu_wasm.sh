#!/bin/bash
# Build the QEMU block layer for the wasm target (browser/node bundle).
#
# Applies patches/qemu/ (emscripten support) to $QEMU_ROOT idempotently,
# configures with the emscripten toolchain, and builds the static archives
# consumed by build_anyfs_wasm.sh Phase 3:
#   libblock.a libio.a libqom.a libauthz.a libcrypto.a
#   libevent-loop-base.a libqemuutil.a
#
# Inputs (env overrides beat build.config.toml via lib/config.sh):
#   QEMU_ROOT     QEMU source tree          (default: paths.qemu_src)
#   EMSDK_DIR     emsdk install root        (default: toolchains.emsdk / $EMSDK)
#   WASM_SYSROOT  wasm static-libs sysroot  (default: paths.wasm_sysroot)
#   BLD           build dir                 (default: $QEMU_ROOT/build-anyfs-wasm)
#
# Usage: ./build_qemu_wasm.sh [-j N] [-h|--help]
set -euo pipefail

# shellcheck source=lib/config.sh
source "$(dirname "$0")/lib/config.sh"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

QEMU_ROOT="${QEMU_ROOT:-$ANYFS_PATHS_QEMU_SRC}"
EMSDK_DIR="${EMSDK_DIR:-$ANYFS_TOOLCHAINS_EMSDK}"
SYS="${WASM_SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
BLD="${BLD:-$QEMU_ROOT/build-anyfs-wasm}"
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)         echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -f "$QEMU_ROOT/configure" ]] || { echo "no QEMU tree at $QEMU_ROOT" >&2; exit 1; }
[[ -d "$SYS/lib" ]] || { echo "no wasm sysroot at $SYS (run fetch_wasm_sysroot.sh)" >&2; exit 1; }

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

# ── Apply patches/qemu (idempotent, same mechanism as oot_fs.sh) ──────
for p in "$REPO_ROOT"/patches/qemu/*.patch; do
    name="$(basename "$p")"
    if (cd "$QEMU_ROOT" && patch -p1 --dry-run --silent < "$p") >/dev/null 2>&1; then
        (cd "$QEMU_ROOT" && patch -p1 --silent < "$p")
        echo "applied qemu patch: $name"
    elif (cd "$QEMU_ROOT" && patch -p1 -R --dry-run --silent < "$p") >/dev/null 2>&1; then
        echo "qemu patch already applied: $name"
    else
        echo "qemu patch $name neither applies forward nor is already applied" >&2
        exit 1
    fi
done

# ── Configure (skipped when the build dir is already configured) ─────
# argv frozen from the v11.0.0 reference build's config.status; on a QEMU
# version bump, reconfigure once by hand and re-derive (meson introspect
# --buildoptions / meson-logs/meson-log.txt records the effective -D options).
if [[ ! -f "$BLD/config-host.mak" ]]; then
    mkdir -p "$BLD"
    # zstd resolves via pkg-config from the wasm sysroot; bz2 via
    # find_library through --extra-ldflags (-L$SYS/lib). The -s emcc link
    # flags live in configs/meson/emscripten.txt (patch 0007).
    (cd "$BLD" && \
        CC=emcc CXX=em++ AR=emar RANLIB=emranlib \
        CFLAGS="-DEMSCRIPTEN" \
        NM="$EMSDK_DIR/upstream/bin/llvm-nm" \
        PKG_CONFIG_LIBDIR="$SYS/lib/pkgconfig" \
        "$QEMU_ROOT/configure" \
            --cpu=wasm32 --static --cross-prefix= \
            --disable-system --disable-user --disable-tools \
            --disable-guest-agent --disable-docs \
            --disable-gtk --disable-sdl --disable-opengl --disable-vnc \
            --disable-spice --disable-gnutls --disable-blkio --disable-numa \
            --disable-cap-ng --disable-seccomp --disable-libssh \
            --disable-curl --disable-rbd --disable-glusterfs --disable-vde \
            --disable-nettle --disable-gcrypt --disable-smartcard \
            --disable-usb-redir --disable-libudev --disable-fuse \
            --disable-libiscsi --disable-libnfs --disable-pixman \
            --disable-png --enable-bzip2 --enable-zstd \
            --disable-tcg --disable-tcg-interpreter \
            --target-list= \
            --extra-cflags="-I$SYS/include" \
            --extra-ldflags="-L$SYS/lib")
fi

# ── Build exactly the archives build_anyfs_wasm.sh consumes ──────────
ARCHIVES=(libblock.a libio.a libqom.a libauthz.a libcrypto.a
          libevent-loop-base.a libqemuutil.a)
ninja -C "$BLD" -j "$JOBS" "${ARCHIVES[@]}"

for a in "${ARCHIVES[@]}"; do
    [[ -f "$BLD/$a" ]] || { echo "missing built archive: $BLD/$a" >&2; exit 1; }
done
echo "OK: QEMU wasm archives in $BLD"
