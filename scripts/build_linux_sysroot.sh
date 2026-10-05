#!/usr/bin/env bash
# Static dependency sysroot for the linux-amd64 build.
#
# Builds every third-party library the linux-amd64 artifacts link as a static,
# -fPIC archive with scripts/lib/zig-cc (target x86_64-linux-gnu.2.11: the
# glibc floor, baseline x86-64), and installs headers, archives and .pc files
# into $SYSROOT (paths.linux_sysroot in build.config.toml). Consumers build
# with PKG_CONFIG_LIBDIR pointing only at $SYSROOT/lib/pkgconfig, so a host
# library can never leak in. Versions come from scripts/lib/sysroot_sources.sh
# (shared with the wasm sysroot). The Electron addons link these same
# archives at their 2.25 floor: objects carry no symbol versions, the final
# link picks them.
#
# Usage:
#   ./scripts/build_linux_sysroot.sh                 # everything
#   ./scripts/build_linux_sysroot.sh --only=curl     # one recipe (needs its deps)
#   ./scripts/build_linux_sysroot.sh --clean         # wipe $SYSROOT and the work dir first
#
# Env overrides: SYSROOT, WORK, JOBS.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

SYSROOT="${SYSROOT:-$ANYFS_PATHS_LINUX_SYSROOT}"
WORK="${WORK:-$REPO_ROOT/build-linux-sysroot}"
JOBS="${JOBS:-$(nproc)}"
PATCHES="$REPO_ROOT/patches/sysroot"

ONLY=""
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --only=*) ONLY="${arg#--only=}" ;;
        --clean)  CLEAN=1 ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0 ;;
        *) echo "unknown argument: $arg (try --only=<lib>, --clean)" >&2; exit 1 ;;
    esac
done

for tool in meson ninja pkg-config perl make curl python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool not on PATH" >&2; exit 1; }
done
[[ -x "$REPO_ROOT/.toolchain/zig/zig" ]] || { echo "zig not installed: run scripts/fetch_zig.sh" >&2; exit 1; }

[[ $CLEAN -eq 1 ]] && rm -rf "$WORK" "$SYSROOT"
mkdir -p "$WORK" "$SYSROOT/lib/pkgconfig" "$SYSROOT/include"

# shellcheck source=lib/sysroot_sources.sh
source "$SCRIPT_DIR/lib/sysroot_sources.sh"

export CC="$SCRIPT_DIR/lib/zig-cc" CXX="$SCRIPT_DIR/lib/zig-c++"
export ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.11
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
CFLAGS_BASE="-O2 -fPIC"

# Meson native file for every meson recipe: the wrappers, the sysroot as the
# only pkg-config root, and no cmake (its dependency fallback finds host
# packages through /usr/bin/cmake).
cat > "$WORK/native.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
cmake = 'false'

[properties]
pkg_config_libdir = ['$SYSROOT/lib/pkgconfig']
EOF

apply_patch() {  # <dir> <patch>
    patch -d "$1" -p1 --forward --silent < "$PATCHES/$2"
}

# ---------------------------------------------------------------------------
build_zlib() {
    echo "=== zlib $ZLIB_V ==="
    fetch "$ZLIB_URL" "$ZLIB_SHA" "$WORK/zlib-$ZLIB_V.tar.gz"
    unpack "$WORK/zlib-$ZLIB_V.tar.gz" "zlib-$ZLIB_V"
    cd "$WORK/zlib-$ZLIB_V"
    CFLAGS="$CFLAGS_BASE" ./configure --prefix="$SYSROOT" --static
    make -j"$JOBS" libz.a
    make install
}

# bzip2 has no build system worth driving (hard-coded cc tests) and no .pc;
# compile the 7 library sources directly, like the wasm recipe.
build_bzip2() {
    echo "=== bzip2 $BZ2_V ==="
    fetch "$BZ2_URL" "$BZ2_SHA" "$WORK/bzip2-$BZ2_V.tar.gz"
    unpack "$WORK/bzip2-$BZ2_V.tar.gz" "bzip2-$BZ2_V"
    cd "$WORK/bzip2-$BZ2_V"
    local s objs=()
    for s in blocksort bzlib compress crctable decompress huffman randtable; do
        # shellcheck disable=SC2086
        "$CC" $CFLAGS_BASE -D_FILE_OFFSET_BITS=64 -c "$s.c" -o "$s.o"
        objs+=("$s.o")
    done
    rm -f libbz2.a
    ar rcs libbz2.a "${objs[@]}"
    install -m644 libbz2.a "$SYSROOT/lib/libbz2.a"
    install -m644 bzlib.h "$SYSROOT/include/bzlib.h"
}

build_zstd() {
    echo "=== zstd $ZSTD_V ==="
    fetch "$ZSTD_URL" "$ZSTD_SHA" "$WORK/zstd-$ZSTD_V.tar.gz"
    unpack "$WORK/zstd-$ZSTD_V.tar.gz" "zstd-$ZSTD_V"
    cd "$WORK/zstd-$ZSTD_V"
    CFLAGS="$CFLAGS_BASE" make -C lib -j"$JOBS" libzstd.a
    CFLAGS="$CFLAGS_BASE" make -C lib PREFIX="$SYSROOT" \
        install-static install-includes install-pc
}

build_libffi() {
    echo "=== libffi $FFI_V ==="
    fetch "$FFI_URL" "$FFI_SHA" "$WORK/libffi-$FFI_V.tar.gz"
    unpack "$WORK/libffi-$FFI_V.tar.gz" "libffi-$FFI_V"
    cd "$WORK/libffi-$FFI_V"
    CFLAGS="$CFLAGS_BASE" ./configure --host=x86_64-linux-gnu \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --disable-dependency-tracking --disable-multi-os-directory --disable-docs
    make -j"$JOBS"
    make install
}

# glib + pcre2 (forced meson subproject; the wrap file pins and verifies it).
build_glib() {
    echo "=== glib $GLIB_V (+pcre2 subproject) ==="
    fetch "$GLIB_URL" "$GLIB_SHA" "$WORK/glib-$GLIB_V.tar.xz"
    unpack "$WORK/glib-$GLIB_V.tar.xz" "glib-$GLIB_V"
    cd "$WORK/glib-$GLIB_V"
    meson setup _build --native-file "$WORK/native.ini" \
        -Dprefix="$SYSROOT" -Dlibdir=lib \
        -Dbuildtype=release \
        -Ddefault_library=static -Db_staticpic=true \
        -Dforce_fallback_for=pcre2 \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dlibelf=disabled \
        -Dsysprof=disabled -Dintrospection=disabled \
        -Dnls=disabled -Dtests=false -Dman-pages=disabled -Ddocumentation=false \
        -Dglib_debug=disabled
    # glib probes pthread_getname_np with has_header_symbol; zig's headers
    # declare it, but the symbol is glibc 2.12, above the floor, so every glib
    # tool fails to link. Drop the define (config.h is written once at setup).
    # Assert it was there so a glib bump that renames it fails loudly.
    grep -q '#define HAVE_PTHREAD_GETNAME_NP 1' _build/config.h || {
        echo "HAVE_PTHREAD_GETNAME_NP not in _build/config.h — glib changed; revisit this edit" >&2
        exit 1
    }
    sed -i '/#define HAVE_PTHREAD_GETNAME_NP 1/d' _build/config.h
    meson compile -C _build -j "$JOBS"
    meson install -C _build --no-rebuild
}

# libblkid + libuuid from the release tarball (generated configure, so no
# autotools). -Dcrc32c renames util-linux's crc32c: QEMU's libqemuutil.a
# defines a different crc32c (XOR'd result) and both end up in one link —
# duplicate symbol, or VHDX/ext4 checksums silently wrong. __secure_getenv:
# the link probe finds the 2.2.5 compat symbol but current headers don't
# declare it (lib/env.c then fails); without it safe_getenv() still refuses
# setuid callers.
build_blkid() {
    echo "=== util-linux $UL_V (libblkid + libuuid) ==="
    fetch "$UL_URL" "$UL_SHA" "$WORK/util-linux-$UL_V.tar.xz"
    unpack "$WORK/util-linux-$UL_V.tar.xz" "util-linux-$UL_V"
    rm -rf "$WORK/util-linux-build"
    mkdir -p "$WORK/util-linux-build"
    cd "$WORK/util-linux-build"
    CFLAGS="$CFLAGS_BASE -Dcrc32c=anyfs_blkid_crc32c" \
    ac_cv_func___secure_getenv=no \
    "$WORK/util-linux-$UL_V/configure" \
        --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-gnu \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --enable-libblkid --enable-libuuid --disable-all-programs \
        --disable-nls --disable-asciidoc \
        --without-systemd --without-systemdsystemunitdir \
        --without-tinfo --without-readline --without-ncurses --without-ncursesw \
        --without-cap-ng --without-audit --without-libmagic \
        --without-econf --without-cryptsetup \
        --without-util --without-python --without-selinux --without-utempter
    make -j"$JOBS" libblkid.la libuuid.la libblkid/blkid.pc libuuid/uuid.pc
    install -m644 .libs/libblkid.a .libs/libuuid.a "$SYSROOT/lib/"
    mkdir -p "$SYSROOT/include/blkid" "$SYSROOT/include/uuid"
    install -m644 libblkid/src/blkid.h "$SYSROOT/include/blkid/blkid.h"
    install -m644 "$WORK/util-linux-$UL_V/libuuid/src/uuid.h" "$SYSROOT/include/uuid/uuid.h"
    install -m644 libblkid/blkid.pc libuuid/uuid.pc "$SYSROOT/lib/pkgconfig/"
}

# libaio: upstream ships no .pc; write one. CFLAGS must come through the
# environment — src/Makefile does `CFLAGS ?=` then `+= -I. -fPIC`, which a
# command-line CFLAGS would override.
build_libaio() {
    echo "=== libaio $AIO_V ==="
    fetch "$AIO_URL" "$AIO_SHA" "$WORK/libaio-$AIO_V.tar.gz"
    unpack "$WORK/libaio-$AIO_V.tar.gz" "libaio-$AIO_V"
    cd "$WORK/libaio-$AIO_V"
    apply_patch . libaio-0.3.113-static-symver.patch
    CFLAGS="-O2" ENABLE_SHARED=0 make -j"$JOBS"
    CFLAGS="-O2" ENABLE_SHARED=0 make install prefix="$SYSROOT"
    cat > "$SYSROOT/lib/pkgconfig/libaio.pc" <<EOF
prefix=$SYSROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libaio
Description: Linux-native asynchronous I/O access library
URL: https://pagure.io/libaio
Version: $AIO_V
Libs: -L\${libdir} -laio
Cflags: -I\${includedir}
EOF
}

# liburing: --use-libc, because nolibc mode adds -nostdlib to the compile
# flags and zig cc then drops the libc include paths. The archive's objects
# use plain CFLAGS, so -fPIC goes there.
build_liburing() {
    echo "=== liburing $URING_V ==="
    fetch "$URING_URL" "$URING_SHA" "$WORK/liburing-$URING_V.tar.gz"
    unpack "$WORK/liburing-$URING_V.tar.gz" "liburing-liburing-$URING_V"
    cd "$WORK/liburing-liburing-$URING_V"
    CFLAGS="$CFLAGS_BASE" ./configure --cc="$CC" --cxx="$CXX" --use-libc \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" --libdevdir="$SYSROOT/lib" \
        --includedir="$SYSROOT/include" --mandir="$WORK/liburing-man"
    CFLAGS="$CFLAGS_BASE" make -C src -j"$JOBS" ENABLE_SHARED=0
    CFLAGS="$CFLAGS_BASE" make install ENABLE_SHARED=0
}

# libfuse3: --bindir is the compiled-in fusermount3 location. glibc < 2.24
# never reaches libfuse's PATH fallback (posix_spawn reports success and the
# child exits 127), so it must be the real distro path; nothing is installed
# there. disable-libc-symbol-version is upstream's switch for static use.
build_fuse3() {
    echo "=== libfuse $FUSE_V ==="
    fetch "$FUSE_URL" "$FUSE_SHA" "$WORK/fuse-$FUSE_V.tar.gz"
    unpack "$WORK/fuse-$FUSE_V.tar.gz" "fuse-$FUSE_V"
    cd "$WORK/fuse-$FUSE_V"
    apply_patch . fuse-3.18.3-posix-memalign.patch
    meson setup _build --native-file "$WORK/native.ini" \
        --prefix="$SYSROOT" --libdir=lib --bindir=/usr/bin \
        -Dbuildtype=release \
        -Ddefault_library=static -Db_staticpic=true \
        -Dutils=false -Dexamples=false -Dtests=false \
        -Dinitscriptdir= -Denable-io-uring=false \
        -Ddisable-libc-symbol-version=true
    meson compile -C _build -j "$JOBS"
    meson install -C _build --no-rebuild
}

build_openssl() {
    echo "=== OpenSSL $OPENSSL_V ==="
    fetch "$OPENSSL_URL" "$OPENSSL_SHA" "$WORK/openssl-$OPENSSL_V.tar.gz"
    unpack "$WORK/openssl-$OPENSSL_V.tar.gz" "openssl-$OPENSSL_V"
    cd "$WORK/openssl-$OPENSSL_V"
    # OPENSSLDIR=/etc/ssl: the default CA lookup (cert.pem, certs/) matches
    # Debian-family hosts; elsewhere anyfs_tls_ca_init() sets SSL_CERT_FILE.
    perl ./Configure linux-x86_64 \
        --prefix="$SYSROOT" --libdir=lib --openssldir=/etc/ssl \
        no-shared no-module no-tests no-docs no-apps -fPIC
    make -j"$JOBS" build_libs
    make install_dev
}

# curl for QEMU's http(s) block driver. FTP stays: QEMU sets
# CURLOPT_PROTOCOLS_STR "HTTP,HTTPS,FTP,FTPS" and curl rejects the whole list
# if one protocol is compiled out. No CA bundle is compiled in (OpenSSL's
# defaults + SSL_CERT_FILE decide); the distro's openssl.cnf is not loaded,
# since it was written for a different OpenSSL build.
build_curl() {
    echo "=== curl $CURL_V ==="
    fetch "$CURL_URL" "$CURL_SHA" "$WORK/curl-$CURL_V.tar.xz"
    unpack "$WORK/curl-$CURL_V.tar.xz" "curl-$CURL_V"
    cd "$WORK/curl-$CURL_V"
    CFLAGS="-O2" ./configure \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --disable-shared --enable-static --with-pic \
        --disable-dependency-tracking \
        --with-openssl="$SYSROOT" --with-zlib \
        --without-ca-bundle --without-ca-path --with-ca-fallback --without-ca-embed \
        --disable-openssl-auto-load-config \
        --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
        --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
        --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
        --disable-manual --disable-docs \
        --without-libpsl --without-libidn2 --without-brotli --without-zstd \
        --without-nghttp2 --without-nghttp3 --without-ngtcp2 --without-quiche \
        --without-libssh --without-libssh2 --without-libgsasl --without-libuv \
        --without-zsh-functions-dir --without-fish-functions-dir
    make -j"$JOBS"
    make install
}

# ---------------------------------------------------------------------------
# Order: glib needs zlib + libffi; curl needs zlib + OpenSSL.
ALL_LIBS=(zlib bzip2 zstd libffi glib blkid libaio liburing fuse3 openssl curl)

run_one() {
    case "$1" in
        zlib|bzip2|zstd|libffi|glib|blkid|libaio|liburing|fuse3|openssl|curl)
            ( "build_$1" ) ;;
        *) echo "unknown --only target: $1 (one of: ${ALL_LIBS[*]})" >&2; exit 1 ;;
    esac
}

if [[ -n "$ONLY" ]]; then
    run_one "$ONLY"
else
    for lib in "${ALL_LIBS[@]}"; do run_one "$lib"; done
fi

list_libs() { find "$SYSROOT/lib" -maxdepth 1 -name '*.a' -printf '%f\n' | sort; }

echo
echo "=== manifest parity check ($SYSROOT) ==="
if diff <(grep -vE '^#|^$' "$SCRIPT_DIR/lib/linux_sysroot.manifest" | sort) <(list_libs); then
    echo "OK: sysroot lib set matches scripts/lib/linux_sysroot.manifest"
elif [[ -n "$ONLY" ]]; then
    echo "(partial build via --only=$ONLY — parity mismatch expected)"
else
    echo "FAIL: sysroot lib set differs from the manifest" >&2
    exit 1
fi
