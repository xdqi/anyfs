#!/bin/bash
# Tests for elf2dylib.py. A small synthetic shared library is built for arm64
# and x86_64 the way build_kernel_dylib.sh links the kernel; it must convert,
# and the result must pass the independent checks below. Inputs outside the
# accepted shape must be rejected without leaving an output file.
#
# Usage: scripts/macho/test_elf2dylib.sh
# Needs clang, ld.lld, llvm-nm, llvm-objdump, readelf, python3 and zig's
# libSystem.tbd (found next to $ZIG, default /opt/zig/zig).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CLANG="${CLANG:-$(command -v clang-19 || command -v clang)}"
LD="${LD:-$(command -v ld.lld-19 || command -v ld.lld)}"
NM="${NM:-$(command -v llvm-nm-19 || command -v llvm-nm)}"
OBJDUMP="${OBJDUMP:-$(command -v llvm-objdump-19 || command -v llvm-objdump)}"
ZIG="${ZIG:-$(command -v zig || echo /opt/zig/zig)}"
LIBSYSTEM="${LIBSYSTEM:-$(dirname "$(readlink -f "$ZIG")")/lib/libc/darwin/libSystem.tbd}"
[[ -f $LIBSYSTEM ]] || { echo "libSystem.tbd not found at $LIBSYSTEM" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
failures=0
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# The kernel's link flags (build_kernel_dylib.sh); a test may append overrides.
LDFLAGS=(-shared -Bsymbolic -z now -z max-page-size=16384
         -z separate-loadable-segments --no-undefined)

# build ARCH OUT.so [LD-OVERRIDES...] -- SOURCES...
build() {
    local arch="$1" out="$2" target src
    shift 2
    local ldx=() objs=()
    while [[ $1 != -- ]]; do ldx+=("$1"); shift; done
    shift
    local cflags=(-O1 -fPIC -ffreestanding -fno-builtin -fno-stack-protector)
    case "$arch" in
        arm64)  target=aarch64-linux-gnu; cflags+=(-ffixed-x18) ;;
        x86_64) target=x86_64-linux-gnu ;;
    esac
    for src in "$@"; do
        "$CLANG" --target="$target" "${cflags[@]}" -c "$src" -o "$src.$arch.o"
        objs+=("$src.$arch.o")
    done
    "$LD" "${LDFLAGS[@]}" "${ldx[@]}" -o "$out" "${objs[@]}"
}

# convert ARCH IN.so OUT.dylib
convert() {
    python3 "$HERE/elf2dylib.py" --arch "$1" --install-name @rpath/libtest.dylib \
        --libsystem "$LIBSYSTEM" --export exported_add=_lklk_add \
        --export exported_call=_lklk_call -o "$3" "$2"
}

# reject NAME ARCH IN.so
reject() {
    local out="$tmp/$1.$2.dylib"
    if convert "$2" "$3" "$out" > "$tmp/log" 2>&1; then
        fail "$2: $1 was accepted"
    elif [[ -e $out ]]; then
        fail "$2: $1 was rejected but left $out"
    else
        pass "$2: rejects $1: $(grep -m1 'elf2dylib:' "$tmp/log")"
    fi
}

cat > "$tmp/lib_a.c" <<'EOF'
/* Pointers from data into code, rodata, data and bss, plus exports. */
typedef int (*fn_t)(int);
__attribute__((noinline)) static int helper(int x) { return x + 1; }
int other_helper(int x);
int exported_add(int a) { return helper(a) + 41; }
static const char *const words[] = { "alpha", "beta" };
fn_t table[] = { helper, exported_add, other_helper };
int counter;
int *counter_ptr = &counter;
const char *const *words_ptr = words;
int exported_call(int i)
{
	return table[i % 3](i) + *counter_ptr + words_ptr[i & 1][0];
}
EOF
cat > "$tmp/lib_b.c" <<'EOF'
/* A second static function named helper. */
__attribute__((noinline, used)) static int helper(int x) { return x * 2; }
int other_helper(int x) { return helper(x); }
EOF
cat > "$tmp/import.c" <<'EOF'
int counter; /* RW data, so the image has all four PT_LOADs */
int ext_fn(int);
int exported_add(int a) { return ext_fn(a); }
int exported_call(int i) { return i; }
EOF
cat > "$tmp/textrel.c" <<'EOF'
int counter;
int exported_add(int a) { return a; }
int exported_call(int i) { return i; }
__asm__(".text\n.p2align 3\n.globl textref\ntextref:\n.quad counter\n");
EOF
cat > "$tmp/x18.c" <<'EOF'
int counter; /* RW data, so the image has all four PT_LOADs */
int exported_add(int a)
{
	int r;

	__asm__ volatile("mov %w0, w18" : "=r"(r));
	return a + r;
}
int exported_call(int i) { return i; }
EOF

for arch in arm64 x86_64; do
    so="$tmp/ok.$arch.so" out="$tmp/ok.$arch.dylib"
    build "$arch" "$so" -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    if ! convert "$arch" "$so" "$out" > "$tmp/log" 2>&1; then
        fail "$arch: valid input rejected"; cat "$tmp/log"; continue
    fi
    pass "$arch: converts: $(cat "$tmp/log")"
    rel=$(readelf -rW "$so" | grep -c '_RELATIVE' || true)
    reb=$("$OBJDUMP" --macho --rebase "$out" | grep -c ' pointer$' || true)
    if [[ $rel -gt 0 && $rel -eq $reb ]]; then pass "$arch: $reb rebases for $rel RELATIVE relocations"
    else fail "$arch: $reb rebases for $rel RELATIVE relocations"; fi
    exports=$("$OBJDUMP" --macho --exports-trie "$out" | awk '/^0x/ { print $2 }' | LC_ALL=C sort | tr '\n' ' ')
    if [[ $exports == "_lklk_add _lklk_call " ]]; then pass "$arch: exports $exports"
    else fail "$arch: exports are '$exports'"; fi
    locals=$("$NM" -m "$out" | awk '$3 == "non-external" { print $4 }' | LC_ALL=C sort | tr '\n' ' ')
    if [[ $locals == *"_helper _helper~2 "* ]]; then pass "$arch: both static helpers kept, renamed apart"
    else fail "$arch: local symbols are '$locals'"; fi
    if [[ $("$NM" -u "$out") == dyld_stub_binder ]]; then pass "$arch: only dyld_stub_binder undefined"
    else fail "$arch: undefined symbols: $("$NM" -u "$out" | tr '\n' ' ')"; fi

    build "$arch" "$tmp/import.$arch.so" -z undefs -- "$tmp/import.c"
    reject import "$arch" "$tmp/import.$arch.so"
    build "$arch" "$tmp/textrel.$arch.so" -z notext -- "$tmp/textrel.c"
    reject textrel "$arch" "$tmp/textrel.$arch.so"
    build "$arch" "$tmp/page4k.$arch.so" -z max-page-size=4096 -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    reject page4k "$arch" "$tmp/page4k.$arch.so"
done
build arm64 "$tmp/x18.arm64.so" -- "$tmp/x18.c"
reject x18 arm64 "$tmp/x18.arm64.so"

if [[ $failures -eq 0 ]]; then echo "PASS test_elf2dylib"; else echo "FAILED: $failures"; exit 1; fi
