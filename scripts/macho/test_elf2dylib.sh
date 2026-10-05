#!/bin/bash
# Tests for elf2dylib.py. A small synthetic shared library is built for arm64
# and x86_64 the way build_kernel_dylib.sh links the kernel; it must convert,
# and the result must pass the independent checks below. Inputs outside the
# accepted shape must be rejected for the expected reason, and a stale output
# file must be gone afterwards.
#
# Usage: scripts/macho/test_elf2dylib.sh
# Needs clang, ld.lld, llvm-nm, llvm-objdump, readelf, python3 and zig's
# libSystem.tbd (found next to $ZIG, default /opt/zig/zig).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=macos_target.sh
source "$HERE/macos_target.sh"
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

# convert ARCH IN.so OUT.dylib [ELF2DYLIB-ARGS...], at ARCH's deployment
# target (macos_target.sh).
convert() {
    local min
    min="$(macos_min "$1")"
    python3 "$HERE/elf2dylib.py" --arch "$1" --install-name @rpath/libtest.dylib \
        --min-os "$min" --libsystem "$LIBSYSTEM" --export exported_add=_lklk_add \
        --export exported_call=_lklk_call "${@:4}" -o "$3" "$2"
}

# accept NAME ARCH IN.so: IN.so must convert and pass the independent checks.
accept() {
    local name="$1" arch="$2" so="$3" out="${3%.so}.dylib" rel reb exports locals elf mach
    if ! convert "$arch" "$so" "$out" > "$tmp/log" 2>&1; then
        fail "$arch: $name: valid input rejected"; cat "$tmp/log"; return
    fi
    pass "$arch: $name: converts: $(cat "$tmp/log")"
    rel=$(readelf -rW "$so" | grep -c '_RELATIVE' || true)
    reb=$("$OBJDUMP" --macho --rebase "$out" | grep -c ' pointer$' || true)
    if [[ $rel -gt 0 && $rel -eq $reb ]]; then pass "$arch: $name: $reb rebases for $rel RELATIVE relocations"
    else fail "$arch: $name: $reb rebases for $rel RELATIVE relocations"; fi
    exports=$("$OBJDUMP" --macho --exports-trie "$out" | awk '/^0x/ { print $2 }' | LC_ALL=C sort | tr '\n' ' ')
    if [[ $exports == "_lklk_add _lklk_call " ]]; then pass "$arch: $name: exports $exports"
    else fail "$arch: $name: exports are '$exports'"; fi
    locals=$("$NM" -m "$out" | awk '$3 == "non-external" { print $4 }' | LC_ALL=C sort | tr '\n' ' ')
    if [[ $locals == *"_helper _helper~2 "* ]]; then pass "$arch: $name: both static helpers kept, renamed apart"
    else fail "$arch: $name: local symbols are '$locals'"; fi
    if [[ $("$NM" -u "$out") == dyld_stub_binder ]]; then pass "$arch: $name: only dyld_stub_binder undefined"
    else fail "$arch: $name: undefined symbols: $("$NM" -u "$out" | tr '\n' ' ')"; fi
    elf=$("$NM" -D --defined-only "$so" | awk '$3 == "exported_add" { print $1 }')
    mach=$("$NM" "$out" | awk '$3 == "_lklk_add" { print $1 }')
    if [[ $elf =~ ^[0-9a-f]+$ && $mach =~ ^[0-9a-f]+$ ]] && (( 16#$mach == 16#$elf + 0x4000 )); then
        pass "$arch: $name: _lklk_add at 0x$mach is exported_add + 0x4000"
    else fail "$arch: $name: _lklk_add at '$mach', exported_add at '$elf'"; fi
}

# reject NAME ARCH IN.so REASON [ELF2DYLIB-ARGS...]: IN.so must be rejected
# with a message containing REASON, and the stale output file must be removed.
reject() {
    local out="$tmp/$1.$2.dylib" msg
    echo stale > "$out"
    if convert "$2" "$3" "$out" "${@:5}" > "$tmp/log" 2>&1; then
        fail "$2: $1 was accepted"
    elif [[ -e $out ]]; then
        fail "$2: $1 was rejected but left $out"
    else
        msg="$(sed -n '/^elf2dylib:/,$p' "$tmp/log")"
        if [[ $msg == *"$4"* ]]; then pass "$2: rejects $1: $(head -1 <<< "$msg")"
        else fail "$2: $1 was rejected, but not for '$4': $(grep -m1 'elf2dylib:' "$tmp/log" || tail -1 "$tmp/log")"; fi
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
cat > "$tmp/nonrel.c" <<'EOF'
int counter; /* RW data, so the image has all four PT_LOADs */
extern int v;
int *p = &v; /* an absolute relocation against an import */
int exported_add(int a) { return a; }
int exported_call(int i) { return i; }
EOF
cat > "$tmp/ctor.c" <<'EOF'
int counter; /* RW data, so the image has all four PT_LOADs */
__attribute__((constructor)) static void init_counter(void)
{
	__asm__ volatile(""); /* keeps clang from running it at compile time */
	counter = 1;
}
int exported_add(int a) { return a + counter; }
int exported_call(int i) { return i; }
EOF

for arch in arm64 x86_64; do
    build "$arch" "$tmp/ok.$arch.so" -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    accept ok "$arch" "$tmp/ok.$arch.so"
    # lld's default max-page-size on AArch64: segments 64 KiB apart.
    build "$arch" "$tmp/page64k.$arch.so" -z max-page-size=65536 -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    accept page64k "$arch" "$tmp/page64k.$arch.so"

    build "$arch" "$tmp/import.$arch.so" -z undefs -- "$tmp/import.c"
    reject import "$arch" "$tmp/import.$arch.so" "PLT relocations"
    build "$arch" "$tmp/nonrel.$arch.so" -z undefs -- "$tmp/nonrel.c"
    reject nonrel "$arch" "$tmp/nonrel.$arch.so" "only R_*_RELATIVE"
    build "$arch" "$tmp/textrel.$arch.so" -z notext -- "$tmp/textrel.c"
    reject textrel "$arch" "$tmp/textrel.$arch.so" "DT_TEXTREL"
    build "$arch" "$tmp/page4k.$arch.so" -z max-page-size=4096 -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    reject page4k "$arch" "$tmp/page4k.$arch.so" "16 KiB boundary"
    build "$arch" "$tmp/relr.$arch.so" -z pack-relative-relocs -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    reject relr "$arch" "$tmp/relr.$arch.so" "DT_RELR"
    build "$arch" "$tmp/android.$arch.so" --pack-dyn-relocs=android -- "$tmp/lib_a.c" "$tmp/lib_b.c"
    reject android "$arch" "$tmp/android.$arch.so" "unsupported dynamic tag 0x60000011"
    build "$arch" "$tmp/ctor.$arch.so" -- "$tmp/ctor.c"
    reject ctor "$arch" "$tmp/ctor.$arch.so" "unsupported dynamic tag 0x19"
    reject noexport "$arch" "$tmp/ok.$arch.so" "no such defined dynamic symbol" --export missing=_missing
    reject dupexport "$arch" "$tmp/ok.$arch.so" "more than once" --export exported_add=_lklk_add2
    reject dupmacho "$arch" "$tmp/ok.$arch.so" "exported_add is already exported as _lklk_add" \
        --export other_helper=_lklk_add
done
reject archmismatch arm64 "$tmp/ok.x86_64.so" "expected ET_DYN for arm64"
head -c 4096 "$tmp/ok.x86_64.so" > "$tmp/truncated.so"
reject truncated x86_64 "$tmp/truncated.so" "malformed ELF"
build arm64 "$tmp/x18.arm64.so" -- "$tmp/x18.c"
reject x18 arm64 "$tmp/x18.arm64.so" "1 instruction uses x18/w18"

# The x18 scan reads instructions only: neither the file name in objdump's
# header line nor a <symbol> annotation counts.
mkdir "$tmp/x18"
cp "$tmp/lib_a.c" "$tmp/lib_b.c" "$tmp/x18/"
cat > "$tmp/x18/x18name.c" <<'EOF'
/* objdump shows the call as "bl ... <x18>". */
__attribute__((noinline)) static int x18(int x) { return x + 18; }
int call_x18(int x) { return x18(x) * 2; }
EOF
build arm64 "$tmp/x18/ok.arm64.so" -- "$tmp/x18/lib_a.c" "$tmp/x18/lib_b.c" "$tmp/x18/x18name.c"
accept x18-names arm64 "$tmp/x18/ok.arm64.so"

# -o naming the input through another path is rejected, and the input survives.
cp "$tmp/ok.x86_64.so" "$tmp/inplace.so"
if convert x86_64 "$tmp/inplace.so" "$tmp/./inplace.so" > "$tmp/log" 2>&1; then
    fail "x86_64: -o equal to the input was accepted"
elif ! cmp -s "$tmp/ok.x86_64.so" "$tmp/inplace.so"; then
    fail "x86_64: -o equal to the input changed or deleted the input"
elif ! grep -q '^elf2dylib:.*is the input file' "$tmp/log"; then
    fail "x86_64: -o equal to the input was rejected, but not as the input file: $(tail -1 "$tmp/log")"
else
    pass "x86_64: rejects -o equal to the input: $(grep -m1 'elf2dylib:' "$tmp/log")"
fi

# --min-os has no default: the deployment target is the caller's decision.
if python3 "$HERE/elf2dylib.py" --arch x86_64 --install-name @rpath/libtest.dylib \
        --libsystem "$LIBSYSTEM" --export exported_add=_lklk_add \
        -o "$tmp/nominos.dylib" "$tmp/ok.x86_64.so" > "$tmp/log" 2>&1; then
    fail "x86_64: a conversion without --min-os was accepted"
elif ! grep -q 'required: --min-os' "$tmp/log"; then
    fail "x86_64: a conversion without --min-os failed, but not for --min-os: $(tail -1 "$tmp/log")"
else
    pass "x86_64: requires --min-os: $(tail -1 "$tmp/log")"
fi

if [[ $failures -eq 0 ]]; then echo "PASS test_elf2dylib"; else echo "FAILED: $failures"; exit 1; fi
