#!/bin/bash
# Build LKL for the WebAssembly target (browser disk viewer).
#
# Usage: ./build_lkl_wasm.sh [OPTIONS]
#
# Options:
#   --linux=DIR   Kernel source tree (default: ~/linux)
#   --out=DIR     Parent dir containing lkl-wasm/ (default: ~/anyfs-reader)
#   --emsdk=DIR   emsdk install root (default: ~/emsdk)
#   --clean       Run `make clean` before building
#   -j N          Parallelism (default: nproc)
#
# Expects lkl-wasm/ to already have a .config + tools/lkl/Makefile.conf +
# include/lkl_autoconf.h. Generate them with gen_lkl_config_wasm.sh.
#
# Toolchain dispatch:
#   - CC=emcc / AR=emar (Emscripten wraps clang and wasm-ld with the right
#     target/sysroot/libc-stubs)
#   - LD=emcc, since the kernel uses partial linking ($(LD) -r vmlinux); emcc
#     forwards -r to wasm-ld and keeps the output in wasm format
#   - No CROSS_COMPILE (it would prefix the tool names, defeating emcc/emar)
set -e

LINUX_DIR="$HOME/linux"
OUT_PARENT="$HOME/anyfs-reader"
EMSDK_DIR="$HOME/emsdk"
DO_CLEAN=0
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux=*) LINUX_DIR="${1#--linux=}"; shift ;;
        --linux)   LINUX_DIR="$2"; shift 2 ;;
        --out=*)   OUT_PARENT="${1#--out=}"; shift ;;
        --out)     OUT_PARENT="$2"; shift 2 ;;
        --emsdk=*) EMSDK_DIR="${1#--emsdk=}"; shift ;;
        --emsdk)   EMSDK_DIR="$2"; shift 2 ;;
        --clean)   DO_CLEAN=1; shift ;;
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

OUT="$OUT_PARENT/lkl-wasm"
if [[ ! -f "$OUT/.config" ]]; then
    echo "Error: $OUT/.config not found. Run gen_lkl_config_wasm.sh first." >&2
    exit 1
fi
if [[ ! -d "$LINUX_DIR/tools/lkl" ]]; then
    echo "Error: $LINUX_DIR/tools/lkl not found. Is --linux=$LINUX_DIR correct?" >&2
    exit 1
fi

# Pre-generated syscall_defs.h. The wasm assembler silently merges custom
# sections, so the kernel's in-band extraction via objcopy yields an empty
# section. Reuse the mingw32 build's header — same 32-bit syscall ABI and
# same Kconfig-driven __NR_* set as wasm32.
PRESEED_SYSCALL_DEFS_H="$OUT_PARENT/lkl-mingw32/arch/lkl/include/generated/uapi/asm/syscall_defs.h"
if [[ ! -f "$PRESEED_SYSCALL_DEFS_H" ]]; then
    echo "Error: $PRESEED_SYSCALL_DEFS_H not found. Build lkl-mingw32 first." >&2
    exit 1
fi
export PRESEED_SYSCALL_DEFS_H

# Activate emsdk environment so emcc/emar resolve from $PATH.
# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

echo "=============================================================="
echo "  Building lkl-wasm"
echo "  OUT:   $OUT"
echo "  EMSDK: $EMSDK_DIR"
echo "=============================================================="

# The kernel detects CC as clang (emcc reports "Emscripten gcc/clang-like
# replacement") and pulls in scripts/Makefile.clang, which needs
# CLANG_TARGET_FLAGS_$(SRCARCH). Provide it via env so we don't have to edit
# the kernel tree.
export CLANG_TARGET_FLAGS_lkl="wasm32-unknown-emscripten"

# wasm-ld dispatch:
#   1. --start-group/--end-group around $(KBUILD_VMLINUX_LIBS) — LLD already
#      does iterative archive resolution and rejects the flags. Strip them.
#   2. --script=*.lds from the final vmlinux link — emscripten's wasm-ld
#      doesn't speak GNU-ld SECTIONS{}, but Joel Severin's patched LLVM
#      (linux-wasm fork) does. We need the script to run so the kernel's
#      bracket symbols (__setup_start, init_thread_union, ...) actually get
#      materialised into vmlinux.unstripped. Route the script link to Joel's
#      wasm-ld; everything else still goes through emsdk's stock wasm-ld.
JOEL_WASM_LD="$HOME/linux-wasm/workspace/install/llvm/bin/wasm-ld"
if [[ ! -x "$JOEL_WASM_LD" ]]; then
    echo "Error: Joel's wasm-ld not found at $JOEL_WASM_LD" >&2
    echo "Build it with: cd ~/linux-wasm && ./linux-wasm.sh build-llvm" >&2
    exit 1
fi
LD_WRAPPER="$OUT/wasm-ld-wrapper"
mkdir -p "$OUT"
cat > "$LD_WRAPPER" <<EOF
#!/bin/sh
have_script=0
for arg in "\$@"; do
    case "\$arg" in
        # tools/lkl/Makefile.autoconf probes \`\$LD -r -print-output-format\`
        # to detect host platform (NT_HOSTS / POSIX_HOSTS / KASAN_HOSTS).
        # None of those gates fire for wasm, but the probe noise pollutes the
        # build log. Short-circuit with the format we'd report (wasm32).
        -print-output-format) echo wasm32; exit 0 ;;
        --script=*.lds)       have_script=1 ;;
    esac
done
# scripts/Makefile.modpost \`touch .vmlinux.export.o\` when CONFIG_OUTPUT_FORMAT
# is non-elf (line 152). wasm-ld rejects 0-byte input. The export table is
# empty anyway since we build with __DISABLE_EXPORTS; drop the file from argv.
filtered=
for arg in "\$@"; do
    case "\$arg" in
        --start-group|--end-group) continue ;;
        *.vmlinux.export.o|*/.vmlinux.export.o)
            [ -s "\$arg" ] || continue ;;
    esac
    filtered="\$filtered \$arg"
done
if [ "\$have_script" = 1 ]; then
    # Joel's wasm-ld supports GNU-ld SECTIONS{} + -r; emsdk's does not.
    # shellcheck disable=SC2086
    exec "$JOEL_WASM_LD" \$filtered
fi
# shellcheck disable=SC2086
exec "${EMSDK_DIR}/upstream/bin/wasm-ld" \$filtered
EOF
chmod +x "$LD_WRAPPER"

# OBJCOPY wrapper. llvm-objcopy on wasm only supports section dumping,
# removal, and addition. scripts/Makefile.vmlinux's `strip_relocs` step
# invokes objcopy twice, with the second call passing
# `-w --strip-unneeded-symbol=...` which llvm-objcopy rejects on wasm. We
# intercept that exact shape (single input that's already the output, with
# --strip-unneeded-symbol present) and make it a no-op. arch/lkl's own
# objcopy calls (-R / -j / --set-section-flags / --prefix-symbols=empty)
# fall through to real llvm-objcopy.
OBJCOPY_WRAPPER="$OUT/llvm-objcopy-wrapper"
cat > "$OBJCOPY_WRAPPER" <<'EOF'
#!/bin/sh
# scripts/Makefile.vmlinux's cmd_strip_relocs hits llvm-objcopy with two
# invocations that wasm objects don't support:
#   1. --set-section-flags X=noload  vmlinux.unstripped vmlinux  (in→out copy)
#   2. --remove-section=X -w --strip-unneeded-symbol=...  vmlinux  (in-place)
# Detect each shape and short-circuit. arch/lkl's own objcopy calls
# (-R / -j / -O binary / --set-section-flags X=alloc) pass through.
skip_kind=
prev=
for arg in "$@"; do
    case "$arg" in
        --set-section-flags=*=noload) skip_kind=copy ;;
        --strip-unneeded-symbol=*)    skip_kind=inplace ;;
    esac
    if [ "$prev" = "--set-section-flags" ]; then
        case "$arg" in *=noload) skip_kind=copy ;; esac
    fi
    prev="$arg"
done
if [ -n "$skip_kind" ]; then
    case "$skip_kind" in
        copy)
            # Last two non-flag args are input, output.
            files=
            for arg in "$@"; do
                case "$arg" in
                    -*|*=*) ;;
                    *) files="$files $arg" ;;
                esac
            done
            # shellcheck disable=SC2086
            set -- $files
            cp "$1" "$2"
            ;;
        inplace) : ;;
    esac
    exit 0
fi
EOF
echo "exec \"${EMSDK_DIR}/upstream/bin/llvm-objcopy\" \"\$@\"" >> "$OBJCOPY_WRAPPER"
chmod +x "$OBJCOPY_WRAPPER"

# Tools — emcc/emar are python wrappers that call the right clang/lld/llvm-ar
# under emsdk/upstream/bin with the wasm32-emscripten target baked in.
TOOLS=(
    CC="emcc"
    LD="$LD_WRAPPER"
    AR="emar"
    HOSTCC="cc"
    HOSTCXX="c++"
    HOSTLD="ld"
    HOSTAR="ar"
    NM="${EMSDK_DIR}/upstream/bin/llvm-nm"
    OBJCOPY="$OBJCOPY_WRAPPER"
    OBJDUMP="${EMSDK_DIR}/upstream/bin/llvm-objdump"
    READELF="${EMSDK_DIR}/upstream/bin/llvm-readobj"
    STRIP="${EMSDK_DIR}/upstream/bin/llvm-strip"
)

if [[ $DO_CLEAN -eq 1 ]]; then
    OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" ARCH=lkl "${TOOLS[@]}" clean || true
fi

# Target liblkl.a directly — the default `all` target also pulls in the
# hijack LD_PRELOAD libs (POSIX-only, need sys/epoll.h), the FUSE tool, and
# the tests/ binaries, none of which are wanted or buildable for wasm.
#
# KCFLAGS=-D__CYGWIN__: lib/crypto/sha256.c line 275 gates the HMAC-SHA224/256
# functions behind `#if !defined(__DISABLE_EXPORTS) || defined(__CYGWIN__)`.
# arch/lkl/Makefile sets -D__DISABLE_EXPORTS globally for wasm/PE (it's needed
# to suppress EXPORT_SYMBOL's inline-asm that wasm-as rejects), which would
# strip those HMAC functions out — but crypto/sha256.c (the Crypto API
# wrapper) registers HMAC-SHA224/256 unconditionally and references them,
# producing undefined symbols at final link. __CYGWIN__ is only checked in
# this one place in the whole tree (verified by `grep -rn __CYGWIN__
# linux/{lib,crypto,include,kernel}`), so defining it here re-enables the
# HMAC defs without touching anything else.
#
# KCFLAGS=-D__linux__=1: emcc doesn't define __linux__ (target triple is
# wasm32-unknown-emscripten). ZFS source uses
# `#if defined(_KERNEL) && defined(__linux__)` to pick Linux-kernel code
# paths (zfs_file.h's `typedef struct file zfs_file_t`; simd_config.h's
# HAVE_SIMD → HAVE_KERNEL_*). Without this, zfs_file.h falls through to
# `#error "unknown OS"`. Force-define globally — we're building Linux
# kernel code regardless of which userspace runtime hosts the bundle.
OUTPUT="$OUT" make -C "$LINUX_DIR/tools/lkl" -j"$JOBS" ARCH=lkl "${TOOLS[@]}" \
    KCFLAGS="-D__CYGWIN__ -D__linux__=1" \
    "$OUT/tools/lkl/liblkl.a"

# Post-process lkl.o: convert WASM_SYM_ABSOLUTE bracket symbols (emitted by
# Joel's wasm-ld for `name = .` script assignments inside SECTIONS{}) to
# segment-relative DATA symbols. Otherwise emcc's wasm-ld leaves them as
# absolute values that don't match where it actually places the segments,
# and any segment whose only references are these absolute brackets gets
# DCE'd. See wasm_fix_absolute_brackets.py for details.
FIXER="${FIXER:-${LKLFTPD_SRC}/wasm_fix_absolute_brackets.py}"
PREFIXER="${PREFIXER:-${LKLFTPD_SRC}/wasm_prefix_kernel_symbols.py}"
LKLO="$OUT/tools/lkl/lib/lkl.o"
LIBA="$OUT/tools/lkl/liblkl.a"
if [[ -f "$FIXER" ]]; then
    echo
    echo "  FIX  $LKLO (absolute -> segment-relative brackets)"
    python3 "$FIXER" "$LKLO" "$LKLO.fixed" | tail -20
    mv "$LKLO.fixed" "$LKLO"
fi
# Namespace kernel symbols so they stop colliding with libc at final-link.
# Without this, the kernel's vsnprintf / memcpy / etc. outrank musl's weak
# copies and any libc caller in user code ends up running the kernel
# implementation — which prints "(efault)" for any wasm pointer below
# PAGE_SIZE. ELF/PE LKL solves the same problem with `objcopy
# --prefix-symbols=_`; llvm-objcopy advertises that flag but actually
# rejects it on wasm objects, so we do the rewrite ourselves.
if [[ -f "$PREFIXER" ]]; then
    echo
    echo "  NS   $LKLO (prefix kernel symbols)"
    python3 "$PREFIXER" "$LKLO" "$LKLO.prefixed" | tail -10
    mv "$LKLO.prefixed" "$LKLO"
fi
if [[ -f "$LKLO" ]]; then
    "${EMSDK_DIR}/upstream/bin/llvm-ar" rs "$LIBA" "$LKLO" >/dev/null
fi

echo
echo "Output:"
ls -lh "$OUT/tools/lkl/liblkl.a" "$OUT/tools/lkl/lib/lkl.o" 2>/dev/null || true
