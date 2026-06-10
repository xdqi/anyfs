#!/usr/bin/env bash
# Reproducible build recipe for the wasm sysroot.
#
# Rebuilds, from pinned upstream sources, every static library listed in
# scripts/lib/wasm_sysroot.manifest and installs them (plus headers and
# pkgconfig files) into $SYSROOT. Acceptance gate:
#
#   SYSROOT=/tmp/sysroot-rebuild ./scripts/build_wasm_sysroot.sh
#   ls /tmp/sysroot-rebuild/lib/*.a | xargs -n1 basename | sort \
#     | diff <(grep -vE '^#|^$' scripts/lib/wasm_sysroot.manifest | sort) -
#
# Provenance of the known-good hand-built sysroot (reverse-engineered from
# the build trees/logs it preserved under <sysroot>/src and the glib
# checkout's meson-info; snapshot 2026-06-10):
#   - zlib/zstd/libffi: pinned release tarballs, configure lines recovered
#     from the preserved configure.log / config.log.
#   - bzip2: 7 objects named *.c.o in the archive — hand-compiled with emcc
#     (no .pc file, matching upstream bzip2 which ships none).
#   - pcre2 (-8/-16/-32/-posix): NOT built standalone; produced by glib's
#     meson subproject fallback (-Dforce_fallback_for=pcre2), pinned by
#     glib's own subprojects/pcre2.wrap (10.46, sha256 in the wrap file).
#     libpcre2-posix.a is installed but gets no .pc — matches the manifest.
#   - glib 2.88.0 (incl. libgirepository-2.0.a via the default
#     introspection=auto): meson cross build; exact option set recovered
#     from build-wasm32/meson-info/intro-buildoptions.json.
#   - blkid+uuid 2.40.4: util-linux tree via emconfigure (same approach as
#     scripts/build_libblkid_wasm.sh, which remains as the thin
#     blkid-only iteration helper), plus libuuid and the generated .pc
#     files the hand-built sysroot carries.
#   - libresolv.a: a hand-written one-function stub (res_query() returning
#     HOST_NOT_FOUND) so glib/gio's resolver references link; the original
#     source is preserved verbatim below (it was <sysroot>/src/res_query.c).
#
# All libraries are compiled with -O3 -pthread: the final anyfs link is
# pthread-enabled, so every object must carry the atomics/bulk-memory
# target features or wasm-ld rejects the mix.
#
# Usage:
#   ./scripts/build_wasm_sysroot.sh                 # full build into config sysroot
#   SYSROOT=/tmp/sysroot-rebuild ./scripts/build_wasm_sysroot.sh
#   ./scripts/build_wasm_sysroot.sh --only=glib     # iterate one recipe
#   ./scripts/build_wasm_sysroot.sh --clean         # wipe work dir first
#
# Env overrides: SYSROOT, WORK, UL_SRC, EMSDK_ENV.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

SYSROOT="${SYSROOT:-$ANYFS_PATHS_WASM_SYSROOT}"
WORK="${WORK:-$REPO_ROOT/build-wasm-sysroot}"
UL_SRC="${UL_SRC:-$ANYFS_PATHS_UTIL_LINUX}"

ONLY=""
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --only=*) ONLY="${arg#--only=}" ;;
        --clean)  CLEAN=1 ;;
        -h|--help)
            sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown argument: $arg (try --only=<lib>, --clean)" >&2; exit 1 ;;
    esac
done

EMSDK_ENV="${EMSDK_ENV:-${ANYFS_TOOLCHAINS_EMSDK:+$ANYFS_TOOLCHAINS_EMSDK/emsdk_env.sh}}"
if [[ -n "${EMSDK_ENV:-}" && -f "$EMSDK_ENV" ]]; then
    # shellcheck source=/dev/null
    source "$EMSDK_ENV" >/dev/null 2>&1
fi
for tool in emcc emconfigure emmake emar meson ninja pkg-config curl python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool not on PATH — set toolchains.emsdk in build.config.toml (or EMSDK_ENV) and install meson/ninja" >&2
        exit 1
    }
done

[[ $CLEAN -eq 1 ]] && rm -rf "$WORK"
mkdir -p "$WORK" "$SYSROOT/lib/pkgconfig" "$SYSROOT/include"

NPROC="$(nproc)"

# fetch <url> <sha256> <dest> — download (with cache) and verify.
fetch() {
    local url="$1" sha="$2" dest="$3"
    if [[ ! -f "$dest" ]] || ! echo "$sha  $dest" | sha256sum --check --quiet - 2>/dev/null; then
        echo ">>> fetch $url"
        curl -fL --retry 3 -o "$dest" "$url"
    fi
    echo "$sha  $dest" | sha256sum --check --quiet -
}

# unpack <tarball> <dirname> — fresh-extract into $WORK/<dirname>.
unpack() {
    local tarball="$1" dirname="$2"
    rm -rf "${WORK:?}/$dirname"
    tar -xf "$tarball" -C "$WORK"
    [[ -d "$WORK/$dirname" ]] || { echo "expected $dirname after extracting $tarball" >&2; exit 1; }
}

# ---------------------------------------------------------------------------
# zlib 1.3.1
# ---------------------------------------------------------------------------
ZLIB_V=1.3.1
# zlib.net 404s superseded releases; fossils/ archives every version.
ZLIB_URL="https://zlib.net/fossils/zlib-$ZLIB_V.tar.gz"
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
build_zlib() {
    echo "=== zlib $ZLIB_V ==="
    fetch "$ZLIB_URL" "$ZLIB_SHA" "$WORK/zlib-$ZLIB_V.tar.gz"
    unpack "$WORK/zlib-$ZLIB_V.tar.gz" "zlib-$ZLIB_V"
    cd "$WORK/zlib-$ZLIB_V"
    # zlib's configure reads CFLAGS; -pthread keeps the objects compatible
    # with the pthread-enabled final link (atomics target feature).
    CFLAGS="-O3 -pthread" emconfigure ./configure --prefix="$SYSROOT" --static
    emmake make -j"$NPROC" libz.a
    emmake make install   # installs libz.a, zlib.h/zconf.h, lib/pkgconfig/zlib.pc
}

# ---------------------------------------------------------------------------
# bzip2 1.0.8 — upstream has no .pc and its Makefile hardcodes cc tests, so
# compile the 7 library sources directly (the hand-built archive's members
# are exactly these, named *.c.o).
# ---------------------------------------------------------------------------
BZ2_V=1.0.8
BZ2_URL="https://sourceware.org/pub/bzip2/bzip2-$BZ2_V.tar.gz"
BZ2_SHA=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
build_bzip2() {
    echo "=== bzip2 $BZ2_V ==="
    fetch "$BZ2_URL" "$BZ2_SHA" "$WORK/bzip2-$BZ2_V.tar.gz"
    unpack "$WORK/bzip2-$BZ2_V.tar.gz" "bzip2-$BZ2_V"
    cd "$WORK/bzip2-$BZ2_V"
    local srcs=(blocksort bzlib compress crctable decompress huffman randtable)
    local objs=()
    for s in "${srcs[@]}"; do
        emcc -O3 -pthread -D_FILE_OFFSET_BITS=64 -c "$s.c" -o "$s.c.o"
        objs+=("$s.c.o")
    done
    rm -f libbz2.a
    emar rcs libbz2.a "${objs[@]}"
    install -m644 libbz2.a "$SYSROOT/lib/libbz2.a"
    install -m644 bzlib.h "$SYSROOT/include/bzlib.h"
}

# ---------------------------------------------------------------------------
# zstd 1.5.7 — lib/Makefile native targets (the hand-built tree shows the
# obj/conf_*/static layout that Makefile produces).
# ---------------------------------------------------------------------------
ZSTD_V=1.5.7
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v$ZSTD_V/zstd-$ZSTD_V.tar.gz"
ZSTD_SHA=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
build_zstd() {
    echo "=== zstd $ZSTD_V ==="
    fetch "$ZSTD_URL" "$ZSTD_SHA" "$WORK/zstd-$ZSTD_V.tar.gz"
    unpack "$WORK/zstd-$ZSTD_V.tar.gz" "zstd-$ZSTD_V"
    cd "$WORK/zstd-$ZSTD_V"
    CFLAGS="-O3 -pthread" emmake make -C lib -j"$NPROC" libzstd.a
    # install-static/-includes/-pc are plain file copies; PREFIX routes them
    # into the sysroot. (Skip install-shared: static-only sysroot.)
    CFLAGS="-O3 -pthread" emmake make -C lib PREFIX="$SYSROOT" \
        install-static install-includes install-pc
}

# ---------------------------------------------------------------------------
# libffi 3.5.2 — upstream wasm32 support via emconfigure; exact configure
# line recovered from the hand-built tree's config.log.
# ---------------------------------------------------------------------------
FFI_V=3.5.2
FFI_URL="https://github.com/libffi/libffi/releases/download/v$FFI_V/libffi-$FFI_V.tar.gz"
FFI_SHA=f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc
build_libffi() {
    echo "=== libffi $FFI_V ==="
    fetch "$FFI_URL" "$FFI_SHA" "$WORK/libffi-$FFI_V.tar.gz"
    unpack "$WORK/libffi-$FFI_V.tar.gz" "libffi-$FFI_V"
    cd "$WORK/libffi-$FFI_V"
    CFLAGS="-O3 -pthread" emconfigure ./configure \
        --host=wasm32-unknown-linux \
        --prefix="$SYSROOT" \
        --enable-static --disable-shared \
        --disable-dependency-tracking \
        --disable-builddir \
        --disable-multi-os-directory \
        --disable-raw-api \
        --disable-docs
    emmake make -j"$NPROC"
    emmake make install   # libffi.a(+.la), ffi.h/ffitarget.h, libffi.pc
}

# ---------------------------------------------------------------------------
# glib 2.88.0 — meson cross build. Also produces:
#   * libpcre2-{8,16,32,posix}.a + libpcre2-{8,16,32}.pc via the forced
#     pcre2 10.46 subproject fallback (pin lives in glib's
#     subprojects/pcre2.wrap: source_hash 15fbc5ab...299f, wrapdb 10.46-1;
#     meson verifies it on download).
#   * libgirepository-2.0.a (introspection=auto is satisfied cross-side in
#     glib 2.88, which hosts girepository in-tree).
# Depends on zlib + libffi (pkgconfig) AND libresolv (gio hard-requires
# res_query()) already being in $SYSROOT.
# ---------------------------------------------------------------------------
GLIB_V=2.88.0
GLIB_URL="https://download.gnome.org/sources/glib/${GLIB_V%.*}/glib-$GLIB_V.tar.xz"
GLIB_SHA=3546251ccbb3744d4bc4eb48354540e1f6200846572bab68e3a2b7b2b64dfd07
build_glib() {
    echo "=== glib $GLIB_V (+pcre2 subproject, +girepository) ==="
    fetch "$GLIB_URL" "$GLIB_SHA" "$WORK/glib-$GLIB_V.tar.xz"
    unpack "$WORK/glib-$GLIB_V.tar.xz" "glib-$GLIB_V"
    # Emscripten port patch (see the patch header for rationale).
    patch -d "$WORK/glib-$GLIB_V" -p1 \
        < "$SCRIPT_DIR/lib/glib-$GLIB_V-emscripten-fd-query-path.patch"
    # Resolve the cross-file template against this sysroot.
    sed "s|@SYSROOT@|$SYSROOT|g" "$SCRIPT_DIR/lib/emscripten-cross.meson" \
        > "$WORK/cross-wasm32.meson"
    cd "$WORK/glib-$GLIB_V"
    # PKG_CONFIG_LIBDIR (not _PATH) so ONLY the sysroot's .pc files are
    # visible — host libffi/zlib must never leak into the wasm build.
    PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig" meson setup _build \
        --cross-file "$WORK/cross-wasm32.meson" \
        -Dprefix="$SYSROOT" \
        -Dbuildtype=release \
        -Ddefault_library=static \
        -Dforce_fallback_for=pcre2 \
        -Dselinux=disabled \
        -Dxattr=false \
        -Dlibmount=disabled \
        -Dnls=disabled \
        -Dtests=false \
        -Dglib_debug=disabled \
        -Dglib_assert=false \
        -Dglib_checks=false
    # Emscripten's libc DECLARES posix_spawn{,p} and pthread_getname_np (so
    # meson's compile-only checks pass) but does not DEFINE them — the .js
    # tool executables then fail at wasm-ld with undefined symbols. The
    # known-good sysroot was built by dropping the two defines from the
    # generated config.h after setup (verified: its preserved config.h lacks
    # exactly these two lines vs a fresh setup); replicate that. config.h is
    # written at setup time only, so the edit survives the compile.
    sed -i '/#define HAVE_POSIX_SPAWN 1/d;/#define HAVE_PTHREAD_GETNAME_NP 1/d' \
        _build/config.h
    PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig" meson compile -C _build
    meson install -C _build --no-rebuild
}

# ---------------------------------------------------------------------------
# blkid + uuid 2.40.4 — built from the util-linux tree (paths.util_linux in
# build.config.toml), same emconfigure approach as
# scripts/build_libblkid_wasm.sh, extended with libuuid and the generated
# pkgconfig files. The tree must be on the v2.40.4 release (stable/v2.40)
# and have a generated ./configure (run autogen.sh once if not).
# ---------------------------------------------------------------------------
UL_V=2.40.4
build_blkid() {
    echo "=== util-linux $UL_V (libblkid + libuuid) ==="
    if [[ ! -f "$UL_SRC/configure" ]]; then
        echo "util-linux configure not found at $UL_SRC/configure" >&2
        echo "  run \`cd $UL_SRC && ./autogen.sh\` first" >&2
        exit 1
    fi
    local v
    v="$(sed -n "s/^PACKAGE_VERSION='\(.*\)'/\1/p" "$UL_SRC/configure" | head -1)"
    if [[ "$v" != "$UL_V" ]]; then
        echo "util-linux at $UL_SRC is version '$v', manifest pins $UL_V — check out v$UL_V" >&2
        exit 1
    fi
    rm -rf "$WORK/util-linux-build"
    mkdir -p "$WORK/util-linux-build"
    cd "$WORK/util-linux-build"
    # Autoconf runtime probes can't execute cross binaries; emscripten's
    # musl provides these, so pre-seed the cache (see
    # scripts/build_libblkid_wasm.sh for the full rationale).
    export ac_cv_func_openat=yes ac_cv_func_fstatat=yes \
           ac_cv_func_fdopendir=yes ac_cv_func_dirfd=yes
    CFLAGS="-O3 -pthread" emconfigure "$UL_SRC/configure" \
        --host=wasm32-unknown-emscripten \
        --prefix="$SYSROOT" \
        --enable-static --disable-shared \
        --enable-libblkid --enable-libuuid \
        --disable-all-programs \
        --disable-nls --disable-asciidoc \
        --without-systemd --without-systemdsystemunitdir \
        --without-tinfo --without-readline \
        --without-ncurses --without-ncursesw \
        --without-cap-ng --without-audit --without-libmagic \
        --without-libuser --without-econf --without-cryptsetup \
        --without-util --without-python --without-selinux \
        --without-utempter
    emmake make -j"$NPROC" libblkid.la libuuid.la \
        libblkid/blkid.pc libuuid/uuid.pc
    install -m644 .libs/libblkid.a "$SYSROOT/lib/libblkid.a"
    install -m644 .libs/libuuid.a "$SYSROOT/lib/libuuid.a"
    mkdir -p "$SYSROOT/include/blkid" "$SYSROOT/include/uuid"
    # blkid.h is generated (version substituted); uuid.h is plain source.
    if [[ -f libblkid/src/blkid.h ]]; then
        install -m644 libblkid/src/blkid.h "$SYSROOT/include/blkid/blkid.h"
    else
        install -m644 "$UL_SRC/libblkid/src/blkid.h" "$SYSROOT/include/blkid/blkid.h"
    fi
    install -m644 "$UL_SRC/libuuid/src/uuid.h" "$SYSROOT/include/uuid/uuid.h"
    install -m644 libblkid/blkid.pc "$SYSROOT/lib/pkgconfig/blkid.pc"
    install -m644 libuuid/uuid.pc "$SYSROOT/lib/pkgconfig/uuid.pc"
}

# ---------------------------------------------------------------------------
# libresolv — single-function stub. glib/gio reference res_query(); the
# wasm bundle never resolves DNS, so it just reports HOST_NOT_FOUND. This
# is the verbatim source the hand-built sysroot preserved (src/res_query.c).
# ---------------------------------------------------------------------------
build_libresolv() {
    echo "=== libresolv (res_query stub) ==="
    rm -rf "$WORK/libresolv"
    mkdir -p "$WORK/libresolv"
    cd "$WORK/libresolv"
    cat > res_query.c <<'EOF'
#include <netdb.h>
int res_query(const char *name, int class,
              int type, unsigned char *dest, int len)
{
    h_errno = HOST_NOT_FOUND;
    return -1;
}
EOF
    emcc -O3 -pthread -Wno-unused-parameter -c res_query.c -o libresolv.o
    rm -f libresolv.a
    emar rcs libresolv.a libresolv.o
    install -m644 libresolv.a "$SYSROOT/lib/libresolv.a"
}

# ---------------------------------------------------------------------------
# Driver — dependency order matters: glib needs zlib+libffi (pkgconfig from
# this sysroot) and libresolv (gio's res_query check); everything else is
# independent.
# ---------------------------------------------------------------------------
ALL_LIBS=(zlib bzip2 zstd libffi libresolv glib blkid)

run_one() {
    case "$1" in
        zlib)      build_zlib ;;
        bzip2|bz2) build_bzip2 ;;
        zstd)      build_zstd ;;
        libffi|ffi) build_libffi ;;
        glib|pcre2|girepository) build_glib ;;
        blkid|uuid|util-linux) build_blkid ;;
        libresolv|resolv) build_libresolv ;;
        *) echo "unknown --only target: $1 (one of: ${ALL_LIBS[*]})" >&2; exit 1 ;;
    esac
}

if [[ -n "$ONLY" ]]; then
    run_one "$ONLY"
else
    for lib in "${ALL_LIBS[@]}"; do
        run_one "$lib"
    done
fi

list_libs() {
    find "$SYSROOT/lib" -maxdepth 1 -name '*.a' -printf '%f\n' | sort
}

echo
echo "=== sysroot summary ($SYSROOT) ==="
list_libs

echo
echo "=== manifest parity check ==="
if diff <(grep -vE '^#|^$' "$SCRIPT_DIR/lib/wasm_sysroot.manifest" | sort) \
        <(list_libs); then
    echo "OK: sysroot lib set matches scripts/lib/wasm_sysroot.manifest"
elif [[ -n "$ONLY" ]]; then
    echo "(partial build via --only=$ONLY — parity mismatch expected)"
else
    echo "FAIL: sysroot lib set differs from the manifest" >&2
    exit 1
fi
