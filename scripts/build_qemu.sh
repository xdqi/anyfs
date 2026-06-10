#!/bin/bash
# Build QEMU block-layer libraries + libanyfs-qemublk shared object for one or
# more anyfs-reader targets.
#
# Usage: ./build_qemu.sh [OPTIONS]
#
# Options:
#   --qemu-src=DIR      QEMU source tree (default: qemu_src from
#                       build.config.toml; falls back to deps/qemu)
#   --out-prefix=PFX    Build-dir prefix inside qemu-src (default: build-anyfs)
#                       Produces <qemu-src>/<PFX>-<target>/.
#   --targets=LIST      Comma-separated subset of:
#                         linux-amd64,mingw32,mingw64
#                       (default: linux-amd64,mingw32,mingw64)
#   --reconfigure       Wipe build dir and re-run configure
#   --cc=CMD            C compiler override passed to configure as --cc= for
#                       linux-amd64 only; switching compilers needs --reconfigure
#   -j N                Parallelism (default: nproc)
#   -h, --help
#
# Per target the script produces a libanyfs-qemublk.{so,dll} that exposes the
# QEMU block-driver entry points used by anyfs-reader's qemu_backend.c.
# The host pkg-config (linux-amd64) and msys2-cross's per-target wrappers
# (mingw32/mingw64, supplied by msys-cross-pkgconfig) provide glib/zstd/zlib.
#
# Prereqs:
#   - QEMU source tree at $QEMU_SRC (qemu_src in build.config.toml).
#     util/fdmon-poll.c must have 'static' removed from its __thread
#     declarations so a shared object can resolve them — handled by the
#     project's local patch.
#   - For mingw: msys2-cross with mingw-w64-{i686,x86_64}-{glib2,zlib,zstd}
#     installed under <toolchains.msys2_cross>/{mingw32,mingw64}.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/config.sh
source "$SCRIPT_DIR/lib/config.sh"

QEMU_SRC="${QEMU_SRC:-$ANYFS_PATHS_QEMU_SRC}"
OUT_PFX="build-anyfs"
TARGETS_REQ="linux-amd64,mingw32,mingw64"
RECONFIGURE=0
JOBS="$(nproc)"
CC_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qemu-src=*)   QEMU_SRC="${1#--qemu-src=}"; shift ;;
        --qemu-src)     QEMU_SRC="$2"; shift 2 ;;
        --out-prefix=*) OUT_PFX="${1#--out-prefix=}"; shift ;;
        --out-prefix)   OUT_PFX="$2"; shift 2 ;;
        --targets=*)    TARGETS_REQ="${1#--targets=}"; shift ;;
        --targets)      TARGETS_REQ="$2"; shift 2 ;;
        --reconfigure)  RECONFIGURE=1; shift ;;
        --cc=*)         CC_OVERRIDE="${1#--cc=}"; shift ;;
        --cc)           CC_OVERRIDE="$2"; shift 2 ;;
        -j)             JOBS="$2"; shift 2 ;;
        -j*)            JOBS="${1#-j}"; shift ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Resolve QEMU_SRC to absolute: build_one() does `cd "$builddir" && "$QEMU_SRC/configure"`,
# so a relative --qemu-src (e.g. deps/qemu) would break after the cd.
[[ -d "$QEMU_SRC" ]] && QEMU_SRC="$(cd "$QEMU_SRC" && pwd)"

if [[ ! -f "$QEMU_SRC/configure" ]]; then
    echo "Error: $QEMU_SRC/configure not found." >&2
    exit 1
fi

IFS=',' read -ra TARGETS_ARR <<< "$TARGETS_REQ"
for T in "${TARGETS_ARR[@]}"; do
    case "$T" in
        linux-amd64|mingw32|mingw64) ;;
        *) echo "Unknown target: $T" >&2; exit 1 ;;
    esac
done

# Configure args common to every target. Block layer + format drivers only.
COMMON_CONFIGURE=(
    --disable-system --disable-user --enable-tools
    --disable-guest-agent --disable-docs
    --disable-gtk --disable-sdl --disable-opengl --disable-vnc --disable-spice
    --disable-gnutls --disable-blkio --disable-numa
    --disable-cap-ng --disable-seccomp --disable-libssh --enable-curl
    --disable-rbd --disable-glusterfs --disable-vde
    --disable-nettle --disable-gcrypt --disable-smartcard
    --disable-usb-redir --disable-libudev --disable-fuse
    --disable-libiscsi --disable-libnfs
    --target-list=
)

# The static archives we link into libanyfs-qemublk.
QEMU_LIBS=(
    libblock.a
    libqemuutil.a
    libio.a
    libqom.a
    libauthz.a
    libcrypto.a
    libevent-loop-base.a
)

# Per-target configure / link helpers ---------------------------------------

configure_for() {
    # Emits the configure arg list specific to $1, one arg per line so the
    # caller can mapfile it without word-splitting issues.
    local target="$1"
    case "$target" in
        linux-amd64)
            # -fPIC only; -fno-pie at compile time breaks configure's link
            # tests (e.g. memfd_create) because the default crt expects PIE.
            # b_pie=false (set below) handles the final link.
            printf '%s\n' \
                '--extra-cflags=-fPIC'
            ;;
        mingw32)
            printf '%s\n' \
                '--cross-prefix=i686-w64-mingw32-' \
                '--cpu=i386' \
                '--disable-pixman' \
                '--disable-png' \
                "--extra-cflags=-I$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw32/include" \
                "--extra-ldflags=-L$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw32/lib"
            ;;
        mingw64)
            printf '%s\n' \
                '--cross-prefix=x86_64-w64-mingw32-' \
                '--disable-pixman' \
                '--disable-png' \
                "--extra-cflags=-I$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw64/include" \
                "--extra-ldflags=-L$ANYFS_TOOLCHAINS_MSYS2_CROSS/mingw64/lib"
            ;;
    esac
}

# Compiler used to link the shared object.
linker_cc_for() {
    case "$1" in
        linux-amd64) echo "gcc" ;;
        mingw32)     echo "i686-w64-mingw32-gcc" ;;
        mingw64)     echo "x86_64-w64-mingw32-gcc" ;;
    esac
}

# pkg-config command used to resolve glib/zstd/zlib for the shared link.
pkgconfig_for() {
    case "$1" in
        linux-amd64) echo "pkg-config" ;;
        mingw32)     echo "i686-w64-mingw32-pkg-config" ;;
        mingw64)     echo "x86_64-w64-mingw32-pkg-config" ;;
    esac
}

# Output filename of the shared library (relative to the build dir).
shared_artifact_for() {
    case "$1" in
        linux-amd64)     echo "libanyfs-qemublk.so" ;;
        mingw32|mingw64) echo "libanyfs-qemublk.dll" ;;
    esac
}

# Extra (non-pkg-config) libs the link needs.
extra_libs_for() {
    case "$1" in
        linux-amd64)
            # libaio for native AIO, libbz2 for dmg driver, libm because
            # qcow2/parallels reference floating-point helpers.
            printf '%s\n' "-laio" "-lbz2" "-lm"
            ;;
        mingw32|mingw64)
            # No native libaio on Windows; QEMU uses its win32 AIO emulation.
            # Linker needs explicit Win32 import libs because the QEMU configure
            # records those flags for the *final* executables, not for a
            # standalone shared link:
            #   -lpthread        winpthread (clock_gettime, qemu thread shims)
            #   -lpathcch        PathCchSkipRoot (cutils.c)
            #   -lsynchronization WaitOnAddress / WakeByAddress* (futex shim)
            #   -lws2_32/-liphlpapi  network helpers pulled in by URI parsers
            printf '%s\n' \
                "-lpthread" "-lpathcch" "-lsynchronization" \
                "-lws2_32" "-liphlpapi" "-lbz2" "-lm"
            ;;
    esac
}

# Pkg-config modules to resolve for the shared link. `libcurl` is included
# so QEMU's `block/curl.c` (built because --enable-curl is in COMMON_CONFIGURE)
# can resolve curl_* symbols at shared-link time. Adds ~700 KB libcurl-4.dll
# next to the deliverable on mingw64; on Linux distro libcurl is normally
# already on the loader path.
pkg_modules_for() {
    case "$1" in
        linux-amd64)
            # pixman is enabled on Linux, so include it.
            printf '%s\n' "glib-2.0" "gthread-2.0" "zlib" "pixman-1" "libzstd" "libcurl"
            ;;
        mingw32|mingw64)
            # pixman is disabled at configure time for both mingw targets.
            printf '%s\n' "glib-2.0" "gthread-2.0" "zlib" "libzstd" "libcurl"
            ;;
    esac
}

# Build steps ----------------------------------------------------------------

build_one() {
    local target="$1"
    local builddir="$QEMU_SRC/$OUT_PFX-$target"

    echo
    echo "=============================================================="
    echo "  Building QEMU block layer for $target"
    echo "  src:      $QEMU_SRC"
    echo "  builddir: $builddir"
    echo "=============================================================="

    # Configure (fresh or skip if already configured) ------------------------
    if [[ $RECONFIGURE -eq 1 ]]; then
        rm -rf "$builddir"
    fi

    mapfile -t target_cfg < <(configure_for "$target")

    local cc_cfg=()
    [[ -n "$CC_OVERRIDE" && "$target" == "linux-amd64" ]] && cc_cfg=("--cc=$CC_OVERRIDE")

    if [[ ! -f "$builddir/build.ninja" ]]; then
        rm -rf "$builddir"
        mkdir -p "$builddir"
        ( cd "$builddir" && "$QEMU_SRC/configure" \
              "${COMMON_CONFIGURE[@]}" "${target_cfg[@]}" "${cc_cfg[@]}" )
        # b_pie=false matches the -fno-pie/-fPIC flags; needed for the shared
        # link on Linux and harmless on mingw. werror=false keeps the build
        # from tripping over glibc-vs-QEMU prototype drift (e.g.
        # `redundant redeclaration of memfd_create`).
        ( cd "$builddir" && meson configure -Db_pie=false -Dwerror=false ) || true
    fi

    # Build the static libs ---------------------------------------------------
    ( cd "$builddir" && ninja -j"$JOBS" "${QEMU_LIBS[@]}" )

    # Verify all libs are present before attempting the shared link.
    for lib in "${QEMU_LIBS[@]}"; do
        if [[ ! -f "$builddir/$lib" ]]; then
            echo "ERROR: $builddir/$lib missing after ninja" >&2
            return 1
        fi
    done

    # Link the shared object -------------------------------------------------
    local cc pkgcc out
    cc="$(linker_cc_for "$target")"
    pkgcc="$(pkgconfig_for "$target")"
    out="$builddir/$(shared_artifact_for "$target")"

    mapfile -t pkg_mods   < <(pkg_modules_for "$target")
    mapfile -t extra_libs < <(extra_libs_for "$target")

    # pkg-config --libs may return zero entries if a module is missing — fail
    # loudly rather than producing an under-linked DLL.
    local pkg_libs
    pkg_libs="$("$pkgcc" --libs "${pkg_mods[@]}")"

    echo "  linker: $cc"
    echo "  pkg modules: ${pkg_mods[*]}"
    echo "  -> $(basename "$out")"

    # On mingw, QEMU's __thread variables resolve via __emutls_get_address
    # from libgcc_eh.a. The msys2-cross toolchain ships only the static
    # archive (no libgcc_s.dll.a), so -shared-libgcc would fail to find an
    # import library — embed it instead with -static-libgcc and a trailing
    # -lgcc_eh so emutls.o is pulled in after the QEMU libs that reference it.
    local libgcc_flag=() libgcc_trailing=()
    case "$target" in
        mingw32|mingw64)
            libgcc_flag=("-static-libgcc")
            libgcc_trailing=("-lgcc_eh")
            ;;
    esac

    # libblock.a gets --whole-archive so the format driver constructors get
    # pulled in; the rest go in a --start-group so circular refs resolve.
    # shellcheck disable=SC2086
    "$cc" -shared -o "$out" \
        "${libgcc_flag[@]}" \
        -Wl,--whole-archive "$builddir/libblock.a" \
        -Wl,--no-whole-archive \
        -Wl,--start-group \
        "$builddir/libqemuutil.a" \
        "$builddir/libio.a" \
        "$builddir/libqom.a" \
        "$builddir/libauthz.a" \
        "$builddir/libcrypto.a" \
        "$builddir/libevent-loop-base.a" \
        -Wl,--end-group \
        "${libgcc_trailing[@]}" \
        $pkg_libs \
        "${extra_libs[@]}"

    echo "Built: $(ls -lh "$out" | awk '{print $5, $NF}')"
}

FAILED=()
for T in "${TARGETS_ARR[@]}"; do
    if ! build_one "$T"; then
        FAILED+=("$T")
    fi
done

echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo "=== QEMU build complete for: ${TARGETS_ARR[*]} ==="
else
    echo "=== QEMU build FAILED for: ${FAILED[*]} ==="
    exit 1
fi
