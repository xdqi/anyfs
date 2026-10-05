#!/usr/bin/env bash
# Gate for scripts/check_linux_abi.sh, on fixtures built with the pinned zig:
# a binary within the floor passes; one needing a newer glibc, one with a
# non-glibc NEEDED entry, and a shared object with an unversioned undefined
# symbol fail. Skips (77) when zig isn't installed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../scripts/lib/config.sh
source "$root/scripts/lib/config.sh"
zig="$root/.toolchain/zig/zig"
[[ -x "$zig" ]] || { echo "SKIP: zig not installed (scripts/fetch_zig.sh)"; exit 77; }
check="$root/scripts/check_linux_abi.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cc() { local t="$1"; shift; "$zig" cc -target "x86_64-linux-gnu.$t" -g0 "$@"; }
pass() { "$check" "$@" >"$tmp/out" 2>&1 || { cat "$tmp/out"; echo "FAIL: expected pass: $*"; exit 1; }; }
deny() { if "$check" "$@" >"$tmp/out" 2>&1; then cat "$tmp/out"; echo "FAIL: expected failure: $*"; exit 1; fi; }

printf '#include <stdio.h>\nint main(void){puts("x");return 0;}\n' > "$tmp/old.c"
cc 2.11 "$tmp/old.c" -o "$tmp/old"
printf '#include <sys/random.h>\nint main(void){char b[4];return getrandom(b,4,0)<0;}\n' > "$tmp/new.c"
cc 2.25 "$tmp/new.c" -o "$tmp/new"
printf 'int foo(void){return 1;}\n' > "$tmp/foo.c"
cc 2.11 -shared -fPIC "$tmp/foo.c" -o "$tmp/libfoo.so"
printf 'int foo(void);\nint main(void){return foo();}\n' > "$tmp/usefoo.c"
cc 2.11 "$tmp/usefoo.c" -L"$tmp" -lfoo -o "$tmp/usefoo"
# A prototype stands in for a header that declares what the floor lacks.
printf 'int memfd_create(const char *, unsigned int);\nint f(void){return memfd_create("x",0);}\n' > "$tmp/so.c"
cc 2.11 -shared -fPIC "$tmp/so.c" -o "$tmp/libundef.so"

pass 2.11 "$tmp/old"
deny 2.11 "$tmp/new"
pass 2.25 "$tmp/new"
deny 2.11 "$tmp/usefoo"
deny 2.11 "$tmp/libundef.so"
pass --allow-undefined='^memfd_create$' 2.11 "$tmp/libundef.so"
# Directories recurse; non-ELF files are skipped, ELF files still count.
mkdir "$tmp/tree"; cp "$tmp/old" "$tmp/tree/"; echo text > "$tmp/tree/README"
pass 2.11 "$tmp/tree"
cp "$tmp/new" "$tmp/tree/"
deny 2.11 "$tmp/tree"

echo "OK: check_linux_abi.sh enforces floor, NEEDED allowlist and versioned imports"
