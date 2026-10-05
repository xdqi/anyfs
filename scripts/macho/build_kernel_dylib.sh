#!/bin/bash
# Build liblkl-kernel.dylib for macOS from the standard ELF LKL kernel.
#
# Usage: build_kernel_dylib.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR]
#        build_kernel_dylib.sh --universal [--out=DIR]
#
#   --arch       compile lkl_elf_glue.c with the target's ELF compiler, link it
#                with tools/lkl/lib/lkl.o into lkl-kernel.so, and convert that
#                with elf2dylib.py into OUT/<arch>/liblkl-kernel.dylib
#   --lkl-out    LKL build tree (default: <repo>/lkl-linux-arm64 or
#                <repo>/lkl-linux-amd64, as built by build_lkl.sh)
#   --universal  merge OUT/arm64 and OUT/x86_64 into OUT/liblkl-kernel.dylib
#                with llvm-lipo; each slice must stay byte-identical to the
#                per-arch dylib that was checked
#   --out        output root (default: <repo>/build/macos)
#
# Design: docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
LD="${LD:-$(command -v ld.lld-19 || command -v ld.lld)}"
LIPO="${LIPO:-$(command -v llvm-lipo-19 || command -v llvm-lipo)}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"
LIBSYSTEM="${LIBSYSTEM:-$(dirname "$(readlink -f "$ZIG")")/lib/libc/darwin/libSystem.tbd}"

# The ELF/Mach-O boundary. Darwin arm64 departs from AAPCS64 for variadic
# calls, arguments narrower than 32 bits, stack-passed arguments and some
# by-value aggregates. Every entry point below, and every member of
# struct lkl_host_operations, takes at most 8 int/long/enum/pointer
# arguments, none narrower than 32 bits and nothing by value, and none is
# variadic: lkl_start_kernel, lkl_printf and lkl_bug are deliberately absent
# (see lkl_elf_glue.c). Keep it that way when adding to this list.
EXPORTS=(
    lkl_init lkl_cleanup lkl_syscall lkl_sys_halt lkl_is_running
    lkl_get_free_irq lkl_put_irq lkl_trigger_irq
    lkl_glue_set_host lkl_start_kernel_str
)

die() { echo "build_kernel_dylib: $*" >&2; exit 1; }

arch="" lkl_out="" out="$REPO_DIR/build/macos" universal=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch=*)    arch="${1#--arch=}" ;;
        --lkl-out=*) lkl_out="${1#--lkl-out=}" ;;
        --out=*)     out="${1#--out=}" ;;
        --universal) universal=1 ;;
        -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        *)           die "unknown argument: $1" ;;
    esac
    shift
done

if [[ $universal -eq 1 ]]; then
    fat="$out/liblkl-kernel.dylib"
    for slice in arm64 x86_64; do
        [[ -f "$out/$slice/liblkl-kernel.dylib" ]] || die "build --arch=$slice first"
    done
    "$LIPO" -create "$out/arm64/liblkl-kernel.dylib" "$out/x86_64/liblkl-kernel.dylib" \
        -output "$fat.tmp"
    for slice in arm64 x86_64; do
        "$LIPO" -thin "$slice" "$fat.tmp" -output "$fat.$slice"
        if ! cmp -s "$fat.$slice" "$out/$slice/liblkl-kernel.dylib"; then
            rm -f "$fat.tmp" "$fat.$slice"
            die "$slice slice differs from $out/$slice/liblkl-kernel.dylib"
        fi
        rm -f "$fat.$slice"
    done
    mv "$fat.tmp" "$fat"
    echo "build_kernel_dylib: $fat (arm64 + x86_64)"
    exit 0
fi

case "$arch" in
    arm64)  cc=aarch64-linux-gnu-gcc target=linux-arm64
            cflags=(-ffixed-x18 -mno-outline-atomics) ;;
    x86_64) cc=gcc target=linux-amd64 cflags=() ;;
    *)      die "--arch=arm64|x86_64 or --universal is required" ;;
esac
lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
lkl_o="$lkl_out/tools/lkl/lib/lkl.o"
[[ -f $lkl_o ]] || die "$lkl_o not found: run gen_lkl_config.sh and build_lkl.sh --targets=$target"

dir="$out/$arch"
mkdir -p "$dir"
"$cc" -O2 -Wall -fPIC -ffreestanding -fno-builtin -fno-stack-protector "${cflags[@]}" \
    -c "$HERE/lkl_elf_glue.c" -o "$dir/lkl_elf_glue.o"
"$LD" -shared -Bsymbolic -z now -z max-page-size=16384 -z separate-loadable-segments \
    --no-undefined -soname liblkl-kernel.so -o "$dir/lkl-kernel.so" "$lkl_o" "$dir/lkl_elf_glue.o"

export_args=()
for e in "${EXPORTS[@]}"; do
    export_args+=(--export "$e=_lklk_${e#lkl_}")
done
python3 "$HERE/elf2dylib.py" --arch "$arch" --install-name @rpath/liblkl-kernel.dylib \
    --min-os 11.0 --libsystem "$LIBSYSTEM" "${export_args[@]}" \
    -o "$dir/liblkl-kernel.dylib" "$dir/lkl-kernel.so"
