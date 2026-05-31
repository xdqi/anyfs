#!/bin/bash
# Build anyfs-reader for one or more targets, against pre-built LKL trees.
#
# Usage: ./build_anyfs.sh [OPTIONS]
#
# Options:
#   --targets=LIST      Comma-separated subset of:
#                         linux-amd64,mingw32,mingw64
#                       (default: linux-amd64,mingw32,mingw64)
#   --components=LIST   Comma-separated subset of: core,server,fuse
#                       core   = libanyfs_core.a (with qemublk backend if avail)
#                       server = anyfs-ksmbd + anyfs-nfsd + anyfs-lspart
#                       fuse   = anyfs-fuse (Linux only; WinFSP removed)
#                       (default: core,server,fuse)
#   --src=DIR           anyfs-reader source root (default: <script-parent>)
#   --out-prefix=PFX    Build-dir prefix (default: build-anyfs)
#                       Produces <src>/<PFX>-<target>/.
#   --qemu-root=DIR     QEMU source tree (default: ~/qemu)
#   --ksmbd-root=DIR    ksmbd-tools source tree (default: ~/ksmbd-tools)
#   --lkl-src=DIR       Linux kernel source tree (default: ~/linux). Meson
#                       needs this for tools/lkl/include/{lkl.h,lkl_host.h};
#                       the option default in meson_options.txt is the literal
#                       string '${LINUX_SRC}' which cannot be auto-resolved,
#                       so this flag must point at the real tree.
#   --reconfigure       Wipe each build dir and run meson setup fresh
#   -j N                Parallelism (default: nproc)
#   -h, --help
#
# Prereqs: lkl-<target>/tools/lkl/ must already contain liblkl.a/.dll (or .so)
# and Makefile.conf. Build them first with the LKL scripts:
#     scripts/gen_lkl_config.sh --targets=...
#     scripts/build_lkl.sh      --targets=...
#
# Per-target dependencies auto-detected; missing optional pieces are skipped
# with a warning rather than failing the whole build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

TARGETS_REQ="linux-amd64,mingw32,mingw64"
COMPONENTS_REQ="core,server,fuse"
OUT_PFX="build-anyfs"
QEMU_ROOT="$HOME/qemu"
KSMBD_ROOT="$HOME/ksmbd-tools"
LKL_SRC="$HOME/linux"
RECONFIGURE=0
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --targets=*)    TARGETS_REQ="${1#--targets=}"; shift ;;
        --targets)      TARGETS_REQ="$2"; shift 2 ;;
        --components=*) COMPONENTS_REQ="${1#--components=}"; shift ;;
        --components)   COMPONENTS_REQ="$2"; shift 2 ;;
        --src=*)        SRC_DIR="${1#--src=}"; shift ;;
        --src)          SRC_DIR="$2"; shift 2 ;;
        --out-prefix=*) OUT_PFX="${1#--out-prefix=}"; shift ;;
        --out-prefix)   OUT_PFX="$2"; shift 2 ;;
        --qemu-root=*)  QEMU_ROOT="${1#--qemu-root=}"; shift ;;
        --qemu-root)    QEMU_ROOT="$2"; shift 2 ;;
        --ksmbd-root=*) KSMBD_ROOT="${1#--ksmbd-root=}"; shift ;;
        --ksmbd-root)   KSMBD_ROOT="$2"; shift 2 ;;
        --lkl-src=*)    LKL_SRC="${1#--lkl-src=}"; shift ;;
        --lkl-src)      LKL_SRC="$2"; shift 2 ;;
        --reconfigure)  RECONFIGURE=1; shift ;;
        -j)             JOBS="$2"; shift 2 ;;
        -j*)            JOBS="${1#-j}"; shift ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

IFS=',' read -ra TARGETS_ARR  <<< "$TARGETS_REQ"
IFS=',' read -ra COMPONENTS_ARR <<< "$COMPONENTS_REQ"

has_comp() {
    local needle="$1"
    for c in "${COMPONENTS_ARR[@]}"; do
        [[ "$c" == "$needle" ]] && return 0
    done
    return 1
}

# Pre-built QEMU build dirs that expose libanyfs-qemublk.{so,dll}.
# Layout produced by scripts/build_qemu.sh: <qemu_root>/build-anyfs-<target>/.
qemu_build_for() {
    case "$1" in
        linux-amd64|mingw32|mingw64) echo "$QEMU_ROOT/build-anyfs-$1" ;;
        *) return 1 ;;
    esac
}

# Whether the anyfs build should link the QEMU block backend dynamically.
# mingw must use the DLL (avoids IMAGE_REL_AMD64_REL32 overflow from a static
# link); Linux keeps the smaller, self-contained static link.
qemu_shared_for() {
    case "$1" in
        linux-amd64)     echo "false" ;;
        mingw32|mingw64) echo "true" ;;
    esac
}

qemu_artifact_for() {
    # Returns a path under $qbuild that proves the right kind of QEMU output
    # exists for this target — checked before invoking meson so a broken or
    # incomplete QEMU build fails the anyfs step early.
    local target="$1" qbuild="$2"
    case "$(qemu_shared_for "$target")" in
        true)
            case "$target" in
                linux-amd64)     echo "$qbuild/libanyfs-qemublk.so" ;;
                mingw32|mingw64) echo "$qbuild/libanyfs-qemublk.dll" ;;
            esac ;;
        false)
            # Static path needs the QEMU archives; libblock.a is the canary.
            echo "$qbuild/libblock.a" ;;
    esac
}

cross_file_for() {
    case "$1" in
        linux-amd64) echo "" ;;
        mingw32)     echo "$SCRIPT_DIR/cross-anyfs-mingw32.txt" ;;
        mingw64)     echo "$SCRIPT_DIR/cross-anyfs-mingw64.txt" ;;
    esac
}

# Pre-built libblkid archive layout produced by build_libblkid_mingw.sh.
# Returns the directory containing lib/libblkid.a + include/blkid/blkid.h
# when one exists for the target, else empty (meson will fall back to its
# pkg-config probe — which works on linux-amd64 but not on mingw).
blkid_root_for() {
    case "$1" in
        mingw32|mingw64)
            local d="$SRC_DIR/build-blkid-$1"
            [[ -f "$d/lib/libblkid.a" ]] && echo "$d"
            ;;
        linux-amd64|*) echo "" ;;
    esac
}

# Logical component → meson target *names* (as they appear in
# `meson introspect --targets`). We resolve each name to its output
# *filename* below before passing to ninja (ninja wants the file path,
# not the meson logical name).
component_target_names() {
    local target="$1" out=()
    has_comp core   && out+=("anyfs_core")
    if has_comp server; then
        # anyfs-lspart is the discovery companion for ksmbd/nfsd
        # (the servers print "use anyfs-lspart" in their --share help
        # text), so it ships in the same component.
        out+=("anyfs-ksmbd" "anyfs-nfsd" "anyfs-lspart")
    fi
    if has_comp fuse; then
        case "$target" in
            linux-amd64)    out+=("anyfs-fuse") ;;
        esac
    fi
    printf '%s\n' "${out[@]}"
}

build_one() {
    local target="$1"
    local builddir="$SRC_DIR/$OUT_PFX-$target"
    local cross_file
    cross_file="$(cross_file_for "$target")"

    echo
    echo "=============================================================="
    echo "  Building anyfs for $target"
    echo "  src:      $SRC_DIR"
    echo "  builddir: $builddir"
    [[ -n "$cross_file" ]] && echo "  cross:    $cross_file"
    echo "=============================================================="

    # Sanity-check LKL is built. mingw targets link against the DLL
    # (avoids IMAGE_REL_AMD64_REL32 overflow from the static .a, and keeps
    # the executables small); linux-amd64 still links the static archive.
    local lkl_root="$SRC_DIR/lkl-$target/tools/lkl"
    local liblkl_main lkl_shared_opt
    case "$target" in
        linux-amd64)
            liblkl_main="$lkl_root/liblkl.a"
            lkl_shared_opt="false"
            ;;
        mingw32|mingw64)
            liblkl_main="$lkl_root/lib/liblkl.dll"
            lkl_shared_opt="true"
            ;;
    esac
    if [[ ! -f "$liblkl_main" ]]; then
        echo "ERROR: $liblkl_main not found." >&2
        echo "       Build LKL first: scripts/gen_lkl_config.sh + scripts/build_lkl.sh --targets=$target" >&2
        return 1
    fi

    # The kernel source tree is also required (provides tools/lkl/include/lkl.h
    # and arch/lkl/include sources that meson references at configure time).
    if [[ ! -f "$LKL_SRC/tools/lkl/include/lkl.h" ]]; then
        echo "ERROR: --lkl-src='$LKL_SRC' does not look like a Linux kernel tree" >&2
        echo "       (missing tools/lkl/include/lkl.h). Clone tavip/linux there" >&2
        echo "       or pass --lkl-src=PATH explicitly." >&2
        return 1
    fi

    # Per-target options
    local meson_opts=(
        "-Dlkl_dist=$target"
        "-Dlkl_src=$LKL_SRC"
        "-Dlkl_shared=$lkl_shared_opt"
    )

    # ── core: enable QEMU backend if a usable QEMU build is on disk ─
    local qbuild qartifact qshared
    qbuild="$(qemu_build_for "$target")"
    qshared="$(qemu_shared_for "$target")"
    qartifact="$(qemu_artifact_for "$target" "$qbuild")"
    if has_comp core && [[ -n "$qbuild" && -f "$qartifact" ]]; then
        meson_opts+=(
            "-Denable_qemu=true"
            "-Dqemu_root=$QEMU_ROOT"
            "-Dqemu_build=$qbuild"
            "-Dqemu_shared=$qshared"
        )
        echo "  qemu backend: ENABLED (qemu_shared=$qshared, probe=$(basename "$qartifact"))"
    else
        meson_opts+=("-Denable_qemu=false")
        if has_comp core; then
            echo "  qemu backend: SKIPPED ($([[ -z $qbuild ]] && echo 'no QEMU build configured for target' || echo "missing $qartifact"))"
        fi
    fi

    # ── core: hand-built libblkid (mingw cross-builds only) ──────
    # On linux-amd64 we rely on the system pkg-config (meson.build will
    # auto-detect via dependency('blkid')). On mingw we ship our own static
    # archive built from util-linux via scripts/build_libblkid_mingw.sh.
    local blkid_root
    blkid_root="$(blkid_root_for "$target")"
    if has_comp core && [[ -n "$blkid_root" ]]; then
        meson_opts+=("-Dblkid_root=$blkid_root")
        echo "  blkid:        ENABLED ($(basename "$blkid_root"))"
    elif has_comp core; then
        case "$target" in
            mingw32|mingw64)
                echo "  blkid:        SKIPPED (run scripts/build_libblkid_mingw.sh $target first)"
                ;;
        esac
    fi

    # ── server: ksmbd-tools must be on disk ───────────────────────
    if has_comp server; then
        if [[ -d "$KSMBD_ROOT/mountd" ]]; then
            meson_opts+=(
                "-Denable_ksmbd=true"
                "-Dksmbd_tools_root=$KSMBD_ROOT"
            )
        else
            echo "  WARNING: $KSMBD_ROOT/mountd not found — server target disabled."
            meson_opts+=("-Denable_ksmbd=false")
        fi
    else
        meson_opts+=("-Denable_ksmbd=false")
    fi

    # ── fuse: Linux→fuse3 only (WinFSP removed) ────────────────────
    if has_comp fuse; then
        case "$target" in
            linux-amd64)
                if pkg-config --exists fuse3 2>/dev/null; then
                    meson_opts+=("-Denable_fuse=true")
                else
                    echo "  WARNING: fuse3 pkg-config not found — fuse target disabled."
                    meson_opts+=("-Denable_fuse=false")
                fi
                ;;
            mingw32|mingw64)
                meson_opts+=("-Denable_fuse=false")
                ;;
        esac
    else
        meson_opts+=("-Denable_fuse=false")
    fi

    # Cross-file argument (after options so meson sees it as a separate arg list)
    local setup_extra=()
    [[ -n "$cross_file" ]] && setup_extra+=(--cross-file "$cross_file")

    # Configure (fresh or reconfigure)
    if [[ $RECONFIGURE -eq 1 ]]; then
        rm -rf "$builddir"
    fi
    if [[ ! -d "$builddir/meson-info" ]]; then
        ( cd "$SRC_DIR" && meson setup "$builddir" \
              "${setup_extra[@]}" "${meson_opts[@]}" )
    else
        ( cd "$SRC_DIR" && meson configure "$builddir" \
              "${meson_opts[@]}" )
    fi

    # Build only the components we care about.
    mapfile -t want_names < <(component_target_names "$target")
    if [[ ${#want_names[@]} -eq 0 ]]; then
        echo "  (nothing to build for selected components)"
        return 0
    fi

    # Resolve each component name to its output filename(s) via
    # `meson introspect --targets`. Drop names that meson didn't
    # materialize for this configuration.
    local introspect_json
    introspect_json="$(meson introspect --targets "$builddir")"

    local ninja_paths=()
    local out_files=()
    for nm in "${want_names[@]}"; do
        local fns
        fns="$(printf '%s' "$introspect_json" | python3 -c '
import json, sys
name = sys.argv[1]
data = json.load(sys.stdin)
for t in data:
    if t["name"] == name:
        for f in t["filename"]:
            print(f)
        break
' "$nm")"
        if [[ -z "$fns" ]]; then
            echo "  note: meson target '$nm' not in build graph for this config, skipping"
            continue
        fi
        while IFS= read -r f; do
            # Convert absolute path to relative-to-builddir for ninja.
            local rel="${f#$builddir/}"
            ninja_paths+=("$rel")
            out_files+=("$f")
        done <<< "$fns"
    done

    if [[ ${#ninja_paths[@]} -eq 0 ]]; then
        echo "  ERROR: nothing to build (none of the requested components materialized)" >&2
        return 1
    fi

    echo "  ninja targets: ${ninja_paths[*]}"
    if ! ( cd "$builddir" && ninja -j"$JOBS" "${ninja_paths[@]}" ); then
        echo "  ERROR: ninja failed for $target" >&2
        return 1
    fi

    # ── Stage everything (exes + runtime DLLs) into <builddir>/bin/ ──
    stage_bin "$target" "$builddir" "$lkl_root" "$qbuild" "${out_files[@]}"

    echo
    echo "Outputs for $target (in $builddir/bin/):"
    ls -lh "$builddir/bin/" 2>/dev/null
}

# Discover the host sysroot a given mingw target installs DLLs into.
# Returns empty string for linux-amd64.
mingw_sysroot_for() {
    case "$1" in
        mingw32) echo "/opt/msys2-cross/mingw32" ;;
        mingw64) echo "/opt/msys2-cross/mingw64" ;;
        *) echo "" ;;
    esac
}

# Pick the right objdump for inspecting PE imports.
objdump_for() {
    case "$1" in
        mingw32) echo "i686-w64-mingw32-objdump" ;;
        mingw64) echo "x86_64-w64-mingw32-objdump" ;;
        *) echo "" ;;
    esac
}

# Find a runtime DLL named $name in $sysroot/bin (msys2-cross convention).
# Returns absolute path or empty string.
find_sysroot_dll() {
    local name="$1" sysroot="$2"
    local p="$sysroot/bin/$name"
    [[ -f "$p" ]] && { echo "$p"; return; }
    # Some DLLs (libwinpthread, libgcc_s, libstdc++) live alongside the
    # toolchain in lib/ when GCC is the supplier.
    p="$sysroot/lib/$name"
    [[ -f "$p" ]] && { echo "$p"; return; }
    echo ""
}

# Walk a PE's import table and symlink every non-system DLL it imports
# (transitively) from the msys2-cross sysroot into $bin_dir.
stage_pe_imports() {
    local exe="$1" bin_dir="$2" sysroot="$3" objdump="$4"
    local -A seen=()
    local queue=("$exe")
    while [[ ${#queue[@]} -gt 0 ]]; do
        local cur="${queue[0]}"
        queue=("${queue[@]:1}")
        # PE "DLL Name:" entries are exactly the names the loader looks up.
        local imports
        imports=$("$objdump" -p "$cur" 2>/dev/null \
                  | awk '/DLL Name:/ {print $3}')
        local d
        while IFS= read -r d; do
            [[ -z "$d" ]] && continue
            # Skip Windows system DLLs and anything already staged.
            case "${d,,}" in
                kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|ws2_32.dll|\
                iphlpapi.dll|msvcrt.dll|ole32.dll|oleaut32.dll|comdlg32.dll|\
                shell32.dll|comctl32.dll|winspool.drv|uuid.dll|crypt32.dll|\
                userenv.dll|secur32.dll|bcrypt.dll|netapi32.dll|version.dll|\
                shlwapi.dll|dnsapi.dll|psapi.dll|imm32.dll|winmm.dll|\
                rpcrt4.dll|api-ms-win-*|ext-ms-*|ntdll.dll|dbghelp.dll|\
                msimg32.dll|dwmapi.dll|dwrite.dll|usp10.dll|uxtheme.dll|\
                hid.dll|setupapi.dll|cfgmgr32.dll|powrprof.dll)
                    continue ;;
            esac
            [[ -n "${seen[$d]:-}" ]] && continue
            seen[$d]=1
            local src
            src="$(find_sysroot_dll "$d" "$sysroot")"
            if [[ -z "$src" ]]; then
                # Not in msys2-cross — could be the LKL DLL itself (already
                # staged separately) or a user-supplied DLL like WinFSP.
                if [[ ! -f "$bin_dir/$d" ]]; then
                    echo "    note: $d not in $sysroot — caller must supply"
                fi
                continue
            fi
            ln -sfn "$src" "$bin_dir/$d"
            queue+=("$src")
        done <<< "$imports"
    done
}

# Copy/symlink built artifacts + their runtime DLL closure into bin/.
stage_bin() {
    local target="$1" builddir="$2" lkl_root="$3" qbuild="$4"
    shift 4
    local out_files=("$@")
    local bin_dir="$builddir/bin"
    rm -rf "$bin_dir"
    mkdir -p "$bin_dir"

    # All built executables / libraries.
    for f in "${out_files[@]}"; do
        [[ -f "$f" ]] || continue
        ln -sfn "$f" "$bin_dir/$(basename "$f")"
    done

    # For mingw targets, also stage liblkl.dll + transitive DLL closure.
    if [[ "$target" == mingw* ]]; then
        local lkl_dll="$lkl_root/lib/liblkl.dll"
        if [[ -f "$lkl_dll" ]]; then
            ln -sfn "$lkl_dll" "$bin_dir/liblkl.dll"
        fi
        # QEMU block backend DLL, when this target was built with --enable_qemu.
        # The .exes import libanyfs-qemublk.dll directly; stage it from the
        # QEMU build dir so stage_pe_imports can chase its DLL closure.
        if [[ -n "$qbuild" && -f "$qbuild/libanyfs-qemublk.dll" ]]; then
            ln -sfn "$qbuild/libanyfs-qemublk.dll" "$bin_dir/libanyfs-qemublk.dll"
        fi
        local sysroot objdump
        sysroot="$(mingw_sysroot_for "$target")"
        objdump="$(objdump_for "$target")"
        if [[ -n "$sysroot" && -n "$objdump" ]]; then
            # Walk imports for every .exe AND for the staged DLLs that
            # weren't sourced from the sysroot — libanyfs-qemublk.dll and
            # liblkl.dll have their own non-system import chains.
            local probe
            for probe in "$bin_dir"/*.exe "$bin_dir/libanyfs-qemublk.dll" \
                         "$bin_dir/liblkl.dll"; do
                [[ -f "$probe" ]] || continue
                stage_pe_imports "$probe" "$bin_dir" "$sysroot" "$objdump"
            done
        fi
    fi
}

# Validate targets up front
for T in "${TARGETS_ARR[@]}"; do
    case "$T" in
        linux-amd64|mingw32|mingw64) ;;
        *) echo "Unknown target: $T" >&2; exit 1 ;;
    esac
done
# Validate components
for C in "${COMPONENTS_ARR[@]}"; do
    case "$C" in
        core|server|fuse) ;;
        *) echo "Unknown component: $C" >&2; exit 1 ;;
    esac
done

FAILED=()
for T in "${TARGETS_ARR[@]}"; do
    if ! build_one "$T"; then
        FAILED+=("$T")
    fi
done

echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo "=== anyfs build complete for: ${TARGETS_ARR[*]} ==="
else
    echo "=== anyfs build FAILED for: ${FAILED[*]} ==="
    exit 1
fi
