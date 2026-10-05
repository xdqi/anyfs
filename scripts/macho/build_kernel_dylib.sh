#!/bin/bash
# Build liblkl-kernel.dylib for macOS from the standard ELF LKL kernel.
#
# Usage: build_kernel_dylib.sh --arch=arm64|x86_64 [--lkl-out=DIR] [--out=DIR]
#        build_kernel_dylib.sh --universal [--out=DIR]
#
#   --arch       compile lkl_elf_glue.c with the target's ELF compiler, link it
#                with tools/lkl/lib/lkl.o into lkl-kernel.so, and convert that
#                with elf2dylib.py into OUT/<arch>/liblkl-kernel.dylib, for the
#                arch's deployment target in macos_target.sh
#   --lkl-out    LKL build tree (default: <repo>/lkl-linux-arm64 or
#                <repo>/lkl-linux-amd64, as built by build_lkl.sh)
#   --universal  merge OUT/arm64 and OUT/x86_64 into OUT/liblkl-kernel.dylib
#                with llvm-lipo; each slice must stay byte-identical to the
#                per-arch dylib that was checked
#   --out        output root (default: <repo>/build/macos)
#
# Tools: LD (ld.lld), NM (llvm-nm), LIPO (llvm-lipo, --universal only), and
# LIBSYSTEM (default: libSystem.tbd from ZIG's lib dir) override the lookup.
# A failed run leaves none of its outputs behind, nor a stale universal dylib.
#
# Design: docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_DIR/scripts/macho"
# shellcheck source=macos_target.sh
source "$HERE/macos_target.sh"

# The ELF/Mach-O boundary (the spec's "ABI boundary audit"). Darwin arm64
# departs from AAPCS64 for variadic calls, arguments narrower than 32 bits,
# stack-passed arguments and some by-value aggregates. Every entry point
# below, every member of struct lkl_host_operations, and every callback the
# kernel hands the host to call back into ELF code (thread_create's entry
# point, timer_alloc's fn, tls_alloc's destructor, jmp_buf_set's f) takes at
# most 8 int/long/enum/pointer arguments, none narrower than 32 bits and
# nothing by value, and none is variadic: lkl_start_kernel, lkl_printf and
# lkl_bug are deliberately absent (see lkl_elf_glue.c). Return values are
# void, int, long, unsigned long[ long] or pointers: nothing narrower than 32
# bits, no struct, float or union. char signedness differs (unsigned on
# Linux arm64, signed on Darwin), but chars cross only behind pointers.
# jmp_buf_set/jmp_buf_longjmp run Darwin setjmp/longjmp across kernel frames.
# That is safe because both conventions have the same callee-saved registers
# (arm64 x19-x28, d8-d15, fp, lr; x86_64 rbx, rbp, r12-r15), longjmp only
# restores them and unwinds nothing, and the kernel never touches x18. Keep
# all of this true when adding to this list or to lkl_host_operations.
EXPORTS=(
    lkl_init lkl_cleanup lkl_syscall lkl_sys_halt lkl_is_running
    lkl_get_free_irq lkl_put_irq lkl_trigger_irq
    lkl_glue_set_host lkl_start_kernel_str
)
# Every other defined dynamic symbol of lkl-kernel.so. A new one fails the
# build until it is either exported (after the audit above) or listed here.
UNEXPORTED=(lkl_bug lkl_printf lkl_start_kernel)
# START:END pairs of lkl.o's local image markers that must come out in order.
# vmlinux.lds defines several of them between output sections, so in lkl.o
# they are relative to a neighbouring section and land right only if the
# link keeps lkl.o's sections in input order (see the spec's "Kernel link").
BOUNDS=(
    _stext:_etext _sinittext:_einittext __init_begin:__init_end _sdata:_edata
    __start_rodata:__end_rodata __bss_start:__bss_stop
    __start_ro_after_init:__end_ro_after_init __bss_stop:_end
)

die() { echo "build_kernel_dylib: $*" >&2; exit 1; }

# tool VAR CANDIDATE...: print the command in $VAR if set, else the first
# CANDIDATE on PATH; die if there is none. Assign the result to a variable
# (v="$(tool ...)"), so set -e sees a failure.
tool() {
    local var="$1" c
    shift
    local names="$*"
    if [[ -n ${!var:-} ]]; then
        command -v -- "${!var}" || die "$var=${!var} not found"
        return 0
    fi
    for c in "$@"; do
        command -v -- "$c" && return 0
    done
    die "${names// / or } not found on PATH (or set $var)"
}

# zig_libsystem: zig's libSystem.tbd. The lib dir comes from `zig env`, which
# prints JSON ("lib_dir": "...") or, in newer zig, ZON (.lib_dir = "...");
# failing that, it is the lib/ next to the real zig binary.
zig_libsystem() {
    local zig lib
    zig="$(tool ZIG zig /opt/zig/zig)" || exit 1
    lib="$("$zig" env 2>/dev/null \
        | grep -oE '("lib_dir"|\.lib_dir)[[:space:]]*[:=][[:space:]]*"[^"]*"' \
        | head -n 1 | sed -E 's/.*"([^"]*)"$/\1/')" || lib=""
    [[ -n $lib ]] || lib="$(dirname "$(readlink -f "$zig")")/lib"
    echo "$lib/libc/darwin/libSystem.tbd"
}

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
    rm -f "$fat"
    trap 'rm -f "$fat".{tmp,arm64,x86_64}' EXIT
    for slice in arm64 x86_64; do
        [[ -f "$out/$slice/liblkl-kernel.dylib" ]] || die "build --arch=$slice first"
    done
    LIPO="$(tool LIPO llvm-lipo-19 llvm-lipo)"
    "$LIPO" -create "$out/arm64/liblkl-kernel.dylib" "$out/x86_64/liblkl-kernel.dylib" \
        -output "$fat.tmp"
    for slice in arm64 x86_64; do
        "$LIPO" -thin "$slice" "$fat.tmp" -output "$fat.$slice"
        cmp -s "$fat.$slice" "$out/$slice/liblkl-kernel.dylib" \
            || die "$slice slice differs from $out/$slice/liblkl-kernel.dylib"
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
min_os="$(macos_min "$arch")"

# Fail closed: from here on, a failed run leaves none of these behind. The
# universal dylib goes too, as it would no longer match this slice.
dir="$out/$arch"
mkdir -p "$dir"
outputs=("$dir/lkl_elf_glue.o" "$dir/lkl-kernel.so" "$dir/liblkl-kernel.dylib")
rm -f "${outputs[@]}" "$out/liblkl-kernel.dylib"
trap '[[ $? -eq 0 ]] || rm -f "${outputs[@]}"' EXIT

lkl_out="${lkl_out:-$REPO_DIR/lkl-$target}"
lkl_o="$lkl_out/tools/lkl/lib/lkl.o"
[[ -f $lkl_o ]] || die "$lkl_o not found: run gen_lkl_config.sh and build_lkl.sh --targets=$target"
command -v "$cc" > /dev/null || die "$cc not found"
command -v python3 > /dev/null || die "python3 not found"
LD="$(tool LD ld.lld-19 ld.lld)"
NM="$(tool NM llvm-nm-19 llvm-nm)"
LIBSYSTEM="${LIBSYSTEM:-$(zig_libsystem)}"
[[ -f $LIBSYSTEM ]] || die "libSystem.tbd not found at $LIBSYSTEM (set LIBSYSTEM or ZIG)"

"$cc" -O2 -Wall -fPIC -ffreestanding -fno-builtin -fno-stack-protector "${cflags[@]}" \
    -c "$HERE/lkl_elf_glue.c" -o "$dir/lkl_elf_glue.o"
# --unique gives every input section its own output section. Without it, lld
# folds lkl.o's .data..percpu and .data..ro_after_init into one .data, created
# at .data..percpu, so ahead of .rodata, and the BOUNDS markers come out
# inverted. With it, lld's stable sort by segment rank keeps lkl.o's sections
# in input order within each segment.
"$LD" -shared -Bsymbolic -z now -z max-page-size=16384 -z separate-loadable-segments \
    --unique --no-undefined -soname liblkl-kernel.so -o "$dir/lkl-kernel.so" \
    "$lkl_o" "$dir/lkl_elf_glue.o"

declare -A addr=()
while read -r value _ name; do
    [[ -z ${addr[$name]:-} ]] || die "lkl-kernel.so defines the local symbol $name twice"
    addr[$name]=$((16#$value))
done < <("$NM" "$dir/lkl-kernel.so" \
    | awk -v want=" ${BOUNDS[*]//:/ } " 'NF == 3 && $2 ~ /^[a-z]$/ && index(want, " " $3 " ")')
misordered=()
for pair in "${BOUNDS[@]}"; do
    lo="${pair%:*}" hi="${pair#*:}"
    [[ -n ${addr[$lo]:-} && -n ${addr[$hi]:-} ]] \
        || die "lkl-kernel.so lacks the local symbol $lo or $hi"
    start=${addr[$lo]} end=${addr[$hi]}
    (( start <= end )) || misordered+=("$(printf '%s %#x > %s %#x' "$lo" "$start" "$hi" "$end")")
done
if [[ ${#misordered[@]} -gt 0 ]]; then
    list="$(printf '%s, ' "${misordered[@]}")"
    die "lkl-kernel.so has ${list%, }: the link moved lkl.o's sections out of input order"
fi

defined="$("$NM" -D --defined-only "$dir/lkl-kernel.so" | awk '{ print $NF }' | LC_ALL=C sort)"
extra="$(LC_ALL=C comm -23 <(echo "$defined") <(printf '%s\n' "${EXPORTS[@]}" | LC_ALL=C sort))"
want="$(printf '%s\n' "${UNEXPORTED[@]}" | LC_ALL=C sort)"
[[ $extra == "$want" ]] || die "lkl-kernel.so defines {${extra//$'\n'/ }} outside EXPORTS," \
    "expected {${want//$'\n'/ }}: audit each new symbol for the ABI boundary, then add it" \
    "to EXPORTS or UNEXPORTED"

export_args=()
for e in "${EXPORTS[@]}"; do
    export_args+=(--export "$e=_lklk_${e#lkl_}")
done
python3 "$HERE/elf2dylib.py" --arch "$arch" --install-name @rpath/liblkl-kernel.dylib \
    --min-os "$min_os" --libsystem "$LIBSYSTEM" "${export_args[@]}" \
    -o "$dir/liblkl-kernel.dylib" "$dir/lkl-kernel.so"
