#!/usr/bin/env bash
# Gate for scripts/lib/lkl-mingw-cc.sh: kernel compiles go to
# `sccache <cygwin-gcc> -B<as dir>/`, every other call to the mingw gcc name
# resolved through PATH. Uses stub `sccache` and compiler scripts.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
wrapper="$root/scripts/lib/lkl-mingw-cc.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

printf '#!/bin/sh\necho "sccache $*"\n' > "$tmp/sccache"
printf '#!/bin/sh\necho "mingw-name $*"\n' > "$tmp/x86_64-w64-mingw32-gcc"
chmod +x "$tmp/sccache" "$tmp/x86_64-w64-mingw32-gcc"
cyg=/opt/fake/bin/x86_64-pc-cygwin-gcc
asdir=/opt/fake/x86_64-pc-cygwin/bin

check() {
    local want="$1"
    shift
    local got
    got="$(PATH="$tmp:$PATH" "$wrapper" "$cyg" "$asdir" x86_64-w64-mingw32-gcc "$@")"
    [[ "$got" == "$want" ]] || { echo "FAIL: $* -> '$got' (want '$want')"; exit 1; }
}

# Kernel compile: cached/distributed with the cygwin (LP64) compiler, which
# is told where its assembler is (a dist worker can't find it otherwise).
check "sccache $cyg -B$asdir/ -D__KERNEL__ -c a.c -o a.o" -D__KERNEL__ -c a.c -o a.o
# Kernel-side, but not a -c compile (asm-offsets -S, links): shim path.
check "mingw-name -D__KERNEL__ -S b.c -o b.s" -D__KERNEL__ -S b.c -o b.s
check "mingw-name -o vmlinux lkl.o" -o vmlinux lkl.o
# User-space compile and probes: shim / real mingw gcc, as without wrapper.
check "mingw-name -c host.c -o host.o" -c host.c -o host.o
check "mingw-name --version" --version

echo "OK: lkl-mingw-cc.sh routes kernel compiles to sccache + cygwin-gcc"
