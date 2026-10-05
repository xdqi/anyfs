# shellcheck shell=bash disable=SC2034  # the pins are read by the sourcing scripts
# scripts/lib/sysroot_sources.sh — pinned upstream sources for
# scripts/build_wasm_sysroot.sh and scripts/build_linux_sysroot.sh, so a
# library both sysroots carry has one version. Also the download helpers both
# use. Callers set WORK (download + unpack dir) before calling fetch/unpack.

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

# ── both sysroots ────────────────────────────────────────────────────────
ZLIB_V=1.3.1
# zlib.net 404s superseded releases; fossils/ archives every version.
ZLIB_URL="https://zlib.net/fossils/zlib-$ZLIB_V.tar.gz"
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
BZ2_V=1.0.8
BZ2_URL="https://sourceware.org/pub/bzip2/bzip2-$BZ2_V.tar.gz"
BZ2_SHA=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
ZSTD_V=1.5.7
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v$ZSTD_V/zstd-$ZSTD_V.tar.gz"
ZSTD_SHA=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
FFI_V=3.5.2
FFI_URL="https://github.com/libffi/libffi/releases/download/v$FFI_V/libffi-$FFI_V.tar.gz"
FFI_SHA=f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc
GLIB_V=2.88.0
GLIB_URL="https://download.gnome.org/sources/glib/${GLIB_V%.*}/glib-$GLIB_V.tar.xz"
GLIB_SHA=3546251ccbb3744d4bc4eb48354540e1f6200846572bab68e3a2b7b2b64dfd07
# util-linux: the wasm recipe builds the peru checkout (paths.util_linux) and
# checks it is at UL_V; the linux recipe uses the release tarball, which ships
# a generated configure.
UL_V=2.40.4
UL_URL="https://www.kernel.org/pub/linux/utils/util-linux/v${UL_V%.*}/util-linux-$UL_V.tar.xz"
UL_SHA=5c1daf733b04e9859afdc3bd87cc481180ee0f88b5c0946b16fdec931975fb79

# ── linux sysroot only ───────────────────────────────────────────────────
AIO_V=0.3.113
AIO_URL="https://releases.pagure.org/libaio/libaio-$AIO_V.tar.gz"
AIO_SHA=2c44d1c5fd0d43752287c9ae1eb9c023f04ef848ea8d4aafa46e9aedb678200b
URING_V=2.15
URING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_V.tar.gz"
URING_SHA=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
FUSE_V=3.18.3
FUSE_URL="https://github.com/libfuse/libfuse/releases/download/fuse-$FUSE_V/fuse-$FUSE_V.tar.gz"
FUSE_SHA=bcd19582c5e30f7fe45dd86a5540e998590aa01903afc7ebcbeea6c8ac5421ee
OPENSSL_V=3.5.9
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_V/openssl-$OPENSSL_V.tar.gz"
OPENSSL_SHA=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
CURL_V=8.22.0
CURL_URL="https://curl.se/download/curl-$CURL_V.tar.xz"
CURL_SHA=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
