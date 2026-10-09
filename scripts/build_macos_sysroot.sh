#!/usr/bin/env bash
# Static dependency sysroot for the macOS builds, one per arch.
#
# Builds every third-party library the macOS artifacts link (QEMU's block
# layer, anyfs core, the CLIs and the Node addon) as a static archive with zig
# cc (scripts/macho/<arch>-macos-cc, for the arch's deployment target in
# scripts/macho/macos_target.sh), and installs headers, archives and .pc files
# into $SYSROOT/<arch> (paths.macos_sysroot in build.config.toml). Consumers
# resolve dependencies only through $SYSROOT/<arch>/lib/pkgconfig, so nothing
# of the Linux build host can leak in. Versions come from
# scripts/lib/sysroot_sources.sh, shared with the Linux and wasm sysroots.
#
# Compared with the Linux sysroot: no libaio, liburing or libfuse (Linux
# only), and GNU libiconv, because libSystem has no iconv and GLib needs one.
# GLib's libintl comes from its proxy-libintl subproject.
#
# anyfs-fuse links macFUSE's libfuse3, which is not built here: --only=macfuse
# (not part of the default set) takes the fuse3 headers and a .tbd link stub
# of /usr/local/lib/libfuse3.4.dylib from the pinned macFUSE release and
# writes fuse3.pc; build_anyfs.sh then builds anyfs-fuse. Reading the .dmg
# (HFS+) needs the Linux anyfs addon (ANYFS_LINUX_ADDON, default
# ts/packages/anyfs-native/build/Release/anyfs_native.node), plus bsdtar,
# cpio and llvm-readtapi. Running anyfs-fuse needs macFUSE installed.
#
# Usage:
#   ./scripts/build_macos_sysroot.sh --arch=arm64           # everything
#   ./scripts/build_macos_sysroot.sh --arch=x86_64 --only=curl
#   ./scripts/build_macos_sysroot.sh --arch=arm64 --clean   # wipe sysroot + work dir first
#
# Env overrides: SYSROOT (the per-arch directory), WORK, JOBS.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"
# shellcheck source=macho/macos_target.sh
source "$SCRIPT_DIR/macho/macos_target.sh"
# shellcheck source=macho/llvm_tools.sh
source "$SCRIPT_DIR/macho/llvm_tools.sh"

ARCH=""
ONLY=""
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --arch=*) ARCH="${arg#--arch=}" ;;
        --only=*) ONLY="${arg#--only=}" ;;
        --clean)  CLEAN=1 ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0 ;;
        *) echo "unknown argument: $arg (try --arch=arm64|x86_64, --only=<lib>, --clean)" >&2; exit 1 ;;
    esac
done
case "$ARCH" in
    arm64)  CPU=aarch64 ;;
    x86_64) CPU=x86_64 ;;
    *) echo "--arch=arm64|x86_64 is required" >&2; exit 1 ;;
esac
MIN="$(macos_min "$ARCH")"
HOST="$CPU-apple-darwin"

SYSROOT="${SYSROOT:-$ANYFS_PATHS_MACOS_SYSROOT/$ARCH}"
WORK="${WORK:-$REPO_ROOT/build-macos-sysroot/$ARCH}"
JOBS="${JOBS:-$(nproc)}"

for tool in meson ninja pkg-config perl make curl python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool not on PATH" >&2; exit 1; }
done
[[ -x "$REPO_ROOT/.toolchain/zig/zig" ]] || { echo "zig not installed: run scripts/fetch_zig.sh" >&2; exit 1; }

[[ $CLEAN -eq 1 ]] && rm -rf "$WORK" "$SYSROOT"
mkdir -p "$WORK" "$SYSROOT/lib/pkgconfig" "$SYSROOT/include"

# shellcheck source=lib/sysroot_sources.sh
source "$SCRIPT_DIR/lib/sysroot_sources.sh"

export CC="$SCRIPT_DIR/macho/$ARCH-macos-cc" CXX="$SCRIPT_DIR/macho/$ARCH-macos-c++"
export AR="$SCRIPT_DIR/macho/macos-ar" RANLIB="$SCRIPT_DIR/macho/macos-ranlib"
NM="$(llvm_tool llvm-nm)"
export NM
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
CFLAGS_BASE="-O2 -fPIC"
SDK_STUBS="$SCRIPT_DIR/macho/sdk-stubs/Frameworks"

# Meson cross file for every meson recipe: the launchers, the sysroot as the
# only pkg-config root, no cmake (its dependency fallback finds host
# packages through /usr/bin/cmake). GLib wants an ObjC compiler on darwin;
# zig cc compiles ObjC, and with no Apple frameworks GLib's Carbon/Cocoa
# probes fail, so it builds its plain-POSIX code.
cat > "$WORK/cross.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
objc = '$CC'
ar = '$AR'
ranlib = '$RANLIB'
pkg-config = 'pkg-config'
cmake = 'false'

[built-in options]
c_args = ['-I$SYSROOT/include']
c_link_args = ['-L$SYSROOT/lib']
cpp_args = ['-I$SYSROOT/include']
cpp_link_args = ['-L$SYSROOT/lib']
objc_args = ['-I$SYSROOT/include']
objc_link_args = ['-L$SYSROOT/lib']

[properties]
pkg_config_libdir = ['$SYSROOT/lib/pkgconfig']
needs_exe_wrapper = true
# Read by patches/sysroot/glib-2.88.0-darwin-skip-gio.patch.
anyfs_skip_gio = true

[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = '$CPU'
cpu = '$CPU'
endian = 'little'
EOF

apply_patch() {  # <dir> <patch>
    patch -d "$1" -p1 --forward --silent < "$REPO_ROOT/patches/sysroot/$2"
}

# The autoconf recipes all cross-compile: --host plus the Darwin tools.
autoconf_host=(--host="$HOST" --build=x86_64-pc-linux-gnu)

# ---------------------------------------------------------------------------
# zlib's configure picks Apple's libtool as the archiver for any darwin CHOST;
# override AR/ARFLAGS on make's command line.
build_zlib() {
    echo "=== zlib $ZLIB_V ==="
    fetch "$ZLIB_URL" "$ZLIB_SHA" "$WORK/zlib-$ZLIB_V.tar.gz"
    unpack "$WORK/zlib-$ZLIB_V.tar.gz" "zlib-$ZLIB_V"
    cd "$WORK/zlib-$ZLIB_V"
    CHOST="$HOST" CFLAGS="$CFLAGS_BASE" ./configure --prefix="$SYSROOT" --static
    make -j"$JOBS" libz.a AR="$AR" ARFLAGS=rc
    make install AR="$AR" ARFLAGS=rc
}

# bzip2 has no build system worth driving (hard-coded cc tests) and no .pc;
# compile the 7 library sources directly, like the Linux and wasm recipes.
build_bzip2() {
    echo "=== bzip2 $BZ2_V ==="
    fetch "$BZ2_URL" "$BZ2_SHA" "$WORK/bzip2-$BZ2_V.tar.gz"
    unpack "$WORK/bzip2-$BZ2_V.tar.gz" "bzip2-$BZ2_V"
    cd "$WORK/bzip2-$BZ2_V"
    local s objs=()
    for s in blocksort bzlib compress crctable decompress huffman randtable; do
        # shellcheck disable=SC2086
        "$CC" $CFLAGS_BASE -c "$s.c" -o "$s.o"
        objs+=("$s.o")
    done
    rm -f libbz2.a
    "$AR" rcs libbz2.a "${objs[@]}"
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
    CFLAGS="$CFLAGS_BASE" ./configure "${autoconf_host[@]}" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --disable-dependency-tracking --disable-multi-os-directory --disable-docs
    make -j"$JOBS"
    make install
}

build_libiconv() {
    echo "=== libiconv $ICONV_V ==="
    fetch "$ICONV_URL" "$ICONV_SHA" "$WORK/libiconv-$ICONV_V.tar.gz"
    unpack "$WORK/libiconv-$ICONV_V.tar.gz" "libiconv-$ICONV_V"
    cd "$WORK/libiconv-$ICONV_V"
    CFLAGS="$CFLAGS_BASE" ./configure "${autoconf_host[@]}" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --disable-dependency-tracking --disable-nls --enable-extra-encodings=no
    make -j"$JOBS"
    make install
}

# glib + pcre2 (forced meson subproject) + proxy-libintl (glib's own fallback
# when no libintl is found, which is always the case on macOS). The wrap files
# pin and verify both. No gio: zig has no SDK resolver headers or libresolv,
# and nothing anyfs links on macOS uses gio (see the patch).
build_glib() {
    echo "=== glib $GLIB_V (+pcre2, proxy-libintl subprojects; no gio) ==="
    fetch "$GLIB_URL" "$GLIB_SHA" "$WORK/glib-$GLIB_V.tar.xz"
    unpack "$WORK/glib-$GLIB_V.tar.xz" "glib-$GLIB_V"
    cd "$WORK/glib-$GLIB_V"
    apply_patch . glib-2.88.0-darwin-skip-gio.patch
    meson setup _build --cross-file "$WORK/cross.ini" \
        -Dprefix="$SYSROOT" -Dlibdir=lib \
        -Dbuildtype=release \
        -Ddefault_library=static -Db_staticpic=true \
        -Dforce_fallback_for=pcre2 \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dlibelf=disabled \
        -Dsysprof=disabled -Dintrospection=disabled -Ddtrace=disabled \
        -Dnls=disabled -Dtests=false -Dman-pages=disabled -Ddocumentation=false \
        -Dglib_debug=disabled
    meson compile -C _build -j "$JOBS"
    meson install -C _build --no-rebuild
}

# libblkid from the release tarball (generated configure). No libuuid, unlike
# the Linux sysroot: nothing links it, and its uuid_time() is an ELF alias,
# which Mach-O doesn't have. The
# crc32c rename is not optional: QEMU's libqemuutil.a defines its own crc32c
# (a different, XOR'd result) and both land in one static link, where the
# wrong one makes libblkid reject every metadata_csum ext4 superblock — empty
# fstype/label/uuid (docs: project_wasm_blkid_crc32c_collision). The check at
# the end of this recipe refuses an archive that still defines crc32c.
# ac_cv_func_*=no: functions libSystem.tbd exports but zig's Darwin headers
# don't declare (the link probe says yes, the compile then fails).
build_blkid() {
    echo "=== util-linux $UL_V (libblkid) ==="
    fetch "$UL_URL" "$UL_SHA" "$WORK/util-linux-$UL_V.tar.xz"
    unpack "$WORK/util-linux-$UL_V.tar.xz" "util-linux-$UL_V"
    rm -rf "$WORK/util-linux-build"
    mkdir -p "$WORK/util-linux-build"
    cd "$WORK/util-linux-build"
    CFLAGS="$CFLAGS_BASE -Dcrc32c=anyfs_blkid_crc32c" \
    ac_cv_func_getttynam=no \
    "$WORK/util-linux-$UL_V/configure" "${autoconf_host[@]}" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --enable-static --disable-shared --with-pic \
        --enable-libblkid --disable-libuuid --disable-all-programs \
        --disable-nls --disable-asciidoc \
        --without-systemd --without-systemdsystemunitdir \
        --without-tinfo --without-readline --without-ncurses --without-ncursesw \
        --without-cap-ng --without-audit --without-libmagic \
        --without-econf --without-cryptsetup \
        --without-util --without-python --without-selinux --without-utempter
    make -j"$JOBS" libblkid.la libblkid/blkid.pc
    install -m644 .libs/libblkid.a "$SYSROOT/lib/"
    mkdir -p "$SYSROOT/include/blkid"
    install -m644 libblkid/src/blkid.h "$SYSROOT/include/blkid/blkid.h"
    install -m644 libblkid/blkid.pc "$SYSROOT/lib/pkgconfig/"
    check_blkid_crc32c
}

check_blkid_crc32c() {
    local defs
    defs="$("$NM" --defined-only "$SYSROOT/lib/libblkid.a" 2>/dev/null | awk '$NF ~ /crc32c$/ {print $NF}' | sort -u)"
    [[ "$defs" == "_anyfs_blkid_crc32c" ]] || {
        echo "libblkid.a defines {${defs//$'\n'/ }} — expected only _anyfs_blkid_crc32c" >&2
        exit 1
    }
}

build_openssl() {
    echo "=== OpenSSL $OPENSSL_V ==="
    fetch "$OPENSSL_URL" "$OPENSSL_SHA" "$WORK/openssl-$OPENSSL_V.tar.gz"
    unpack "$WORK/openssl-$OPENSSL_V.tar.gz" "openssl-$OPENSSL_V"
    cd "$WORK/openssl-$OPENSSL_V"
    # OPENSSLDIR=/etc/ssl: macOS keeps the system roots in /etc/ssl/cert.pem,
    # OpenSSL's default CA file there.
    local cfg
    case "$ARCH" in
        arm64)  cfg=darwin64-arm64-cc ;;
        x86_64) cfg=darwin64-x86_64-cc ;;
    esac
    perl ./Configure "$cfg" \
        --prefix="$SYSROOT" --libdir=lib --openssldir=/etc/ssl \
        no-shared no-module no-tests no-docs no-apps -fPIC
    make -j"$JOBS" build_libs
    make install_dev
}

# curl for QEMU's http(s) block driver; options as in the Linux sysroot (FTP
# stays: QEMU sets CURLOPT_PROTOCOLS_STR "HTTP,HTTPS,FTP,FTPS"). On macOS curl
# links CoreFoundation, CoreServices and SystemConfiguration; zig has no
# frameworks, so the build finds the stubs in scripts/macho/sdk-stubs.
build_curl() {
    echo "=== curl $CURL_V ==="
    fetch "$CURL_URL" "$CURL_SHA" "$WORK/curl-$CURL_V.tar.xz"
    unpack "$WORK/curl-$CURL_V.tar.xz" "curl-$CURL_V"
    cd "$WORK/curl-$CURL_V"
    CFLAGS="-O2" CPPFLAGS="-F$SDK_STUBS" LDFLAGS="-F$SDK_STUBS" \
    ./configure "${autoconf_host[@]}" \
        --prefix="$SYSROOT" --libdir="$SYSROOT/lib" \
        --disable-shared --enable-static --with-pic \
        --disable-dependency-tracking \
        --with-openssl="$SYSROOT" --with-zlib="$SYSROOT" \
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

# macFUSE: Extras/macFUSE <v>.pkg in the .dmg is a xar archive; its Core
# component's Payload (gzip'd cpio) installs /usr/local/{include/fuse3,lib}.
# The headers default to macFUSE's Darwin extensions (struct fuse_darwin_attr
# in the operations); fuse3.pc turns them off, so anyfs-fuse builds against
# the standard libfuse 3 API that the Linux build uses.
build_macfuse() {
    echo "=== macFUSE $MACFUSE_V (libfuse3 headers + link stub) ==="
    local addon="${ANYFS_LINUX_ADDON:-$REPO_ROOT/ts/packages/anyfs-native/build/Release/anyfs_native.node}"
    [[ -f "$addon" ]] || { echo "macfuse: needs the Linux anyfs addon at $addon to read the .dmg" >&2; exit 1; }
    local t
    for t in bsdtar cpio node; do
        command -v "$t" > /dev/null || { echo "macfuse: $t not on PATH" >&2; exit 1; }
    done
    local readtapi
    readtapi="$(llvm_tool llvm-readtapi)"
    fetch "$MACFUSE_URL" "$MACFUSE_SHA" "$WORK/macfuse-$MACFUSE_V.dmg"
    local d="$WORK/macfuse-$MACFUSE_V"
    rm -rf "$d"
    mkdir -p "$d/xar" "$d/payload"
    node "$SCRIPT_DIR/macho/extract_from_image.mjs" --addon "$addon" \
        "$WORK/macfuse-$MACFUSE_V.dmg" "$d" "Extras/macFUSE $MACFUSE_V.pkg"
    bsdtar -xf "$d/macFUSE $MACFUSE_V.pkg" -C "$d/xar"
    gzip -dc "$d/xar/Core.pkg/PayloadCore.pkg/Payload" | (cd "$d/payload" && cpio -idm --quiet)
    local src="$d/payload/usr/local"
    rm -rf "$SYSROOT/macfuse"
    mkdir -p "$SYSROOT/macfuse/include" "$SYSROOT/macfuse/lib"
    cp -R "$src/include/fuse3" "$SYSROOT/macfuse/include/"
    "$readtapi" --stubify --filetype=tbd-v4 "$src/lib/libfuse3.4.dylib" -o "$SYSROOT/macfuse/lib/libfuse3.tbd"
    grep -q "install-name: *'/usr/local/lib/libfuse3.4.dylib'" "$SYSROOT/macfuse/lib/libfuse3.tbd" \
        || { echo "macfuse: unexpected install name in the libfuse3 stub" >&2; exit 1; }
    cat > "$SYSROOT/lib/pkgconfig/fuse3.pc" <<EOF
prefix=$SYSROOT/macfuse
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: fuse3
Description: macFUSE $MACFUSE_V libfuse3 (link stub; install macFUSE to run)
Version: $(sed -n 's/^Version: //p' "$src/lib/pkgconfig/fuse3.pc")
Libs: -L\${libdir} -lfuse3
Cflags: -I\${includedir} -I\${includedir}/fuse3 -DFUSE_DARWIN_ENABLE_EXTENSIONS=0
EOF
}

# ---------------------------------------------------------------------------
# Order: glib needs zlib + libffi + libiconv; curl needs zlib + OpenSSL.
ALL_LIBS=(zlib bzip2 zstd libffi libiconv glib blkid openssl curl)

run_one() {
    case "$1" in
        zlib|bzip2|zstd|libffi|libiconv|glib|blkid|openssl|curl|macfuse)
            ( "build_$1" ) ;;
        *) echo "unknown --only target: $1 (one of: ${ALL_LIBS[*]} macfuse)" >&2; exit 1 ;;
    esac
}

echo "macOS sysroot: arch=$ARCH (deployment target $MIN) -> $SYSROOT"
if [[ -n "$ONLY" ]]; then
    run_one "$ONLY"
else
    for lib in "${ALL_LIBS[@]}"; do run_one "$lib"; done
fi

list_libs() { find "$SYSROOT/lib" -maxdepth 1 -name '*.a' -printf '%f\n' | sort; }

echo
echo "=== manifest parity check ($SYSROOT) ==="
if diff <(grep -vE '^#|^$' "$SCRIPT_DIR/lib/macos_sysroot.manifest" | sort) <(list_libs); then
    echo "OK: sysroot lib set matches scripts/lib/macos_sysroot.manifest"
elif [[ -n "$ONLY" ]]; then
    echo "(partial build via --only=$ONLY — parity mismatch expected)"
else
    echo "FAIL: sysroot lib set differs from the manifest" >&2
    exit 1
fi
