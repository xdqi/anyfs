#!/usr/bin/env bash
# Gate for scripts/lib/zig-cc.sh: GNU-style -dumpmachine, no host library
# dirs, none of zig's NDEBUG/UBSan/DWARF defaults (an explicit flag still
# wins), the gcc-isms LKL's makefiles use, baseline x86-64 predefines and a
# glibc floor that holds for both targets. Skips (77) when the pinned zig
# isn't installed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../scripts/lib/config.sh
source "$root/scripts/lib/config.sh"
[[ -x "$root/.toolchain/zig/zig" ]] || { echo "SKIP: zig not installed (scripts/fetch_zig.sh)"; exit 77; }
cc="$root/scripts/lib/zig-cc"
cxx="$root/scripts/lib/zig-c++"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

[[ "$("$cc" -dumpmachine)" == x86_64-unknown-linux-gnu ]] || fail "-dumpmachine"
[[ "$(ANYFS_ZIG_TARGET=x86_64-linux-gnu.2.25 "$cc" -dumpmachine)" == x86_64-unknown-linux-gnu ]] \
    || fail "-dumpmachine at 2.25"
if "$cc" -print-search-dirs | grep -q /; then fail "-print-search-dirs lists directories"; fi

printf '#include <assert.h>\nint f(int x){assert(x);return x+1;}\n' > "$tmp/f.c"
"$cc" -O2 -c "$tmp/f.c" -o "$tmp/f.o"
if readelf -S "$tmp/f.o" | grep -q debug_info; then fail "DWARF without -g"; fi
nm "$tmp/f.o" | grep -q __assert_fail || fail "assert() compiled out at -O2"
"$cc" -O2 -g -c "$tmp/f.c" -o "$tmp/g.o"
readelf -S "$tmp/g.o" | grep -q debug_info || fail "explicit -g ignored"
"$cc" -O0 -c "$tmp/f.c" -o "$tmp/u.o"
if nm "$tmp/u.o" | grep -q __ubsan; then fail "UBSan at -O0"; fi
"$cc" -O2 -DNDEBUG -dM -E -x c /dev/null | grep -q '#define NDEBUG' || fail "explicit -DNDEBUG ignored"

# gcc-isms: -Wp,-v (include-dir listing) and -pie together with -shared.
"$cc" -E -Wp,-v -xc /dev/null 2>&1 >/dev/null | grep -q '^ /' || fail "-Wp,-v lists no include dirs"
printf 'int lib(void){return 1;}\n' > "$tmp/lib.c"
"$cc" -fPIC -pie -shared "$tmp/lib.c" -o "$tmp/lib.so" || fail "-pie -shared rejected"
readelf -h "$tmp/lib.so" | grep -q 'DYN' || fail "-pie -shared did not produce a shared object"

printf '#include <stdio.h>\n#include <time.h>\nint main(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);puts("ok");return 0;}\n' > "$tmp/h.c"
printf '#include <cstdio>\n#include <string>\nint main(){std::string s("ok");std::puts(s.c_str());return 0;}\n' > "$tmp/h.cc"
for floor in 2.11 2.25; do
    export ANYFS_ZIG_TARGET="x86_64-linux-gnu.$floor"
    defs="$("$cc" -O2 -dM -E -x c /dev/null)"
    for m in __AVX__ __AVX2__ __SSE4_2__ __SSE3__ NDEBUG; do
        if grep -q "#define $m " <<<"$defs"; then fail "$floor predefines $m"; fi
    done
    grep -q "#define __GLIBC_MINOR__ ${floor#2.}$" <<<"$defs" || fail "$floor: __GLIBC_MINOR__"
    "$cc" -O2 "$tmp/h.c" -o "$tmp/h"
    [[ "$("$tmp/h")" == ok ]] || fail "$floor: C hello"
    "$root/scripts/check_linux_abi.sh" "$floor" "$tmp/h" >/dev/null || fail "$floor: C floor"
    if [[ $floor == 2.25 ]]; then
        # zig builds libc++ on first use; its warnings only matter on failure.
        "$cxx" -O2 "$tmp/h.cc" -o "$tmp/hx" 2>"$tmp/cxx.log" || { cat "$tmp/cxx.log"; fail "C++ build"; }
        [[ "$("$tmp/hx")" == ok ]] || fail "C++ hello"
        "$root/scripts/check_linux_abi.sh" 2.25 "$tmp/hx" >/dev/null || fail "C++ floor/NEEDED"
    fi
done
unset ANYFS_ZIG_TARGET

echo "OK: zig-cc.sh gives gcc-like defaults at the pinned floors"
