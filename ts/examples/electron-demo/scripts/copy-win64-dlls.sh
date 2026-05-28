#!/usr/bin/env bash
# Drop the runtime DLLs needed by anyfs_native.node into the packaged
# Electron app dir, next to anyfs-demo.exe. Windows DLL search resolves
# imports against the loader process's directory — the .node's own dir
# is not on the search path by default.
#
# Mirrors the ship list under $HOME/anyfs-reader/build-anyfs-mingw64/bin/
# (anyfs-ksmbd.exe).
set -euo pipefail

DEST=${1:?usage: copy-win64-dlls.sh <packaged-app-dir>}
[[ -d "$DEST" ]] || { echo "not a dir: $DEST" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

LKL_MINGW64=${LKL_MINGW64:-$REPO_ROOT/lkl-mingw64}
QEMU_BLD_MINGW64=${QEMU_BLD_MINGW64:-$HOME/qemu/build-anyfs-mingw64}
MINGW=${MINGW_SYSROOT:-/opt/msys2-cross/mingw64}

dlls=(
    "$LKL_MINGW64/tools/lkl/lib/liblkl.dll"
    "$QEMU_BLD_MINGW64/libanyfs-qemublk.dll"
    "$MINGW/bin/libwinpthread-1.dll"
    "$MINGW/bin/libglib-2.0-0.dll"
    "$MINGW/bin/libbz2-1.dll"
    "$MINGW/bin/zlib1.dll"
    "$MINGW/bin/libzstd.dll"
    "$MINGW/bin/libpcre2-8-0.dll"
    "$MINGW/bin/libintl-8.dll"
    "$MINGW/bin/libiconv-2.dll"
    "$MINGW/bin/libgcc_s_seh-1.dll"
    "$MINGW/bin/libstdc++-6.dll"
    # libcurl-winssl + its transitive deps. SChannel for TLS so we avoid
    # the OpenSSL stack; idn2/psl for international hostnames; brotli for
    # Content-Encoding; libssh2 for sftp:// (curl pulls it unconditionally).
    "$MINGW/bin/libcurl-4.dll"
    "$MINGW/bin/libssl-3-x64.dll"
    "$MINGW/bin/libcrypto-3-x64.dll"
    "$MINGW/bin/libbrotlidec.dll"
    "$MINGW/bin/libbrotlicommon.dll"
    "$MINGW/bin/libidn2-0.dll"
    "$MINGW/bin/libpsl-5.dll"
    "$MINGW/bin/libssh2-1.dll"
    "$MINGW/bin/libunistring-5.dll"
    "$MINGW/bin/libnghttp2-14.dll"
    "$MINGW/bin/libnghttp3-9.dll"
    "$MINGW/bin/libngtcp2-16.dll"
    "$MINGW/bin/libngtcp2_crypto_ossl-0.dll"
)

for d in "${dlls[@]}"; do
    [[ -f "$d" ]] || { echo "missing: $d" >&2; exit 1; }
done

echo ">>> Copying ${#dlls[@]} runtime DLLs into $DEST"
cp -v "${dlls[@]}" "$DEST/"

# CA certificate bundle for HTTPS (libcurl OpenSSL backend).
CACERT="$MINGW/share/pki/ca-trust-source/ca-bundle.trust.crt"
if [[ -f "$CACERT" ]]; then
    echo ">>> Copying CA bundle: $CACERT -> $DEST/cacert.pem"
    cp -v "$CACERT" "$DEST/cacert.pem"
else
    echo "WARNING: CA bundle not found at $CACERT" >&2
fi

echo "OK — DLLs staged next to anyfs-demo.exe"
