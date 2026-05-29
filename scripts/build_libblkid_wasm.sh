#!/bin/bash
# Build libblkid.a for emscripten and install into the wasm sysroot.
#
# Inputs:
#   - util-linux source tree (default: $HOME/util-linux; override via
#     UL_SRC=...). Must contain a generated `configure` (run autogen.sh once
#     if not).
#   - emsdk on PATH (`source $HOME/emsdk/emsdk_env.sh` before running, or
#     set EMSDK_ENV=/path/to/emsdk_env.sh).
#
# Output:
#   $SYSROOT/lib/libblkid.a
#   $SYSROOT/include/blkid/blkid.h
#   (default sysroot: $HOME/wasm-sysroot)
#
# Why a separate script: build_anyfs_browser_wasm.sh expects libblkid.a +
# blkid.h to already exist in the sysroot; this script provides the recipe
# that produces them. The mingw port lives in patches/libblkid/shim/ and
# uses a hand-compiled source list, but wasm tolerates util-linux's full
# autotools build via emconfigure, so we just drive that.
#
# Usage:
#   ./scripts/build_libblkid_wasm.sh                # uses defaults
#   UL_SRC=/path/to/util-linux SYSROOT=/path/to/sysroot ./scripts/build_libblkid_wasm.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

UL_SRC="${UL_SRC:-$HOME/util-linux}"
SYSROOT="${SYSROOT:-$HOME/wasm-sysroot}"
BLD_DIR="${BLD_DIR:-$REPO_ROOT/build-blkid-wasm}"

if [[ -n "${EMSDK_ENV:-}" && -f "$EMSDK_ENV" ]]; then
    # shellcheck source=/dev/null
    source "$EMSDK_ENV"
fi
if ! command -v emconfigure >/dev/null 2>&1; then
    echo "emconfigure not on PATH — \`source \$HOME/emsdk/emsdk_env.sh\` or set EMSDK_ENV first" >&2
    exit 1
fi

if [[ ! -f "$UL_SRC/configure" ]]; then
    echo "util-linux configure not found at $UL_SRC/configure" >&2
    echo "  run \`cd $UL_SRC && ./autogen.sh\` first" >&2
    exit 1
fi

mkdir -p "$BLD_DIR" "$SYSROOT/lib" "$SYSROOT/include/blkid"
cd "$BLD_DIR"

# emconfigure works by setting CC=emcc / AR=emar / RANLIB=emranlib in the
# environment so the autoconf checks pick them up. Several util-linux
# autoconf probes assume Linux behaviour that emscripten doesn't quite
# match (openat, BSD-style ttyent, /proc/self/mountinfo); we work around
# them by:
#   * --disable-all-programs --enable-libblkid: only build the library,
#     skip every CLI tool (which would pull in much more libc surface).
#   * --enable-static --disable-shared: emscripten can produce side
#     modules but we want a plain .a to feed our final link.
#   * --without-systemd, --disable-nls, --disable-asciidoc:
#     unconditionally off; they would require host-side tooling that
#     doesn't help an embedded archive.
#   * --without-tinfo --without-readline --without-ncurses --without-cap-ng
#     --without-audit --without-libmagic --without-libuser --without-econf
#     --without-cryptsetup --without-util --without-systemdsystemunitdir:
#     same idea — drop probe-only dependencies that just add link work.
#
# Notes on quirks:
#   * configure tests `int openat()` at configure-time. Emscripten exposes
#     it, so HAVE_OPENAT is set and libblkid compiles the sysfs/path code
#     paths. They don't run at our use site (anyfs_probe.c feeds blkid a
#     regular tmpfile path), so this is fine.
#   * The C99 flag is set by util-linux's configure; emcc inherits it.

CONFIGURE_ARGS=(
    --host=wasm32-unknown-emscripten
    --prefix=/usr
    --enable-static --disable-shared
    --enable-libblkid
    --disable-all-programs
    --disable-nls
    --disable-asciidoc
    --without-systemd
    --without-systemdsystemunitdir
    --without-tinfo
    --without-readline
    --without-ncurses
    --without-ncursesw
    --without-cap-ng
    --without-audit
    --without-libmagic
    --without-libuser
    --without-econf
    --without-cryptsetup
    --without-util
    --without-python
    --without-selinux
    --without-utempter
)

# Emscripten's musl ships openat but configure's runtime test would try to
# execute the cross binary — set ac_cv_func_openat=yes upfront so it skips
# the runtime stage. Similar overrides may need to be added if util-linux
# adds more `AC_RUN_IFELSE` probes in future releases.
export ac_cv_func_openat=yes
export ac_cv_func_fstatat=yes
export ac_cv_func_fdopendir=yes
export ac_cv_func_dirfd=yes

echo ">>> emconfigure $UL_SRC/configure ${CONFIGURE_ARGS[*]}"
emconfigure "$UL_SRC/configure" "${CONFIGURE_ARGS[@]}"

echo ">>> emmake make libblkid.la -j$(nproc)"
emmake make -j"$(nproc)" libblkid.la

# libtool produces .libs/libblkid.a (the static archive) — the .la is just
# the libtool descriptor. Install both the archive and the public header.
if [[ -f .libs/libblkid.a ]]; then
    cp .libs/libblkid.a "$SYSROOT/lib/libblkid.a"
else
    echo "libblkid.a not found under .libs/" >&2
    exit 1
fi

# blkid.h is generated from blkid.h.in (autoconf substitutes the version
# string in). After `make`, it lives in libblkid/src/blkid.h relative to
# the build dir; fall back to the source tree if the in-tree build wrote
# it there (autotools VPATH quirk).
if [[ -f libblkid/src/blkid.h ]]; then
    cp libblkid/src/blkid.h "$SYSROOT/include/blkid/blkid.h"
elif [[ -f "$UL_SRC/libblkid/src/blkid.h" ]]; then
    cp "$UL_SRC/libblkid/src/blkid.h" "$SYSROOT/include/blkid/blkid.h"
else
    echo "blkid.h not generated" >&2
    exit 1
fi

echo "Done."
ls -la "$SYSROOT/lib/libblkid.a" "$SYSROOT/include/blkid/blkid.h"
