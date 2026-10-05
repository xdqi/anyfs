#!/usr/bin/env bash
# Gate for scripts/lib/lkl-linux-cc.sh: every call made inside the kernel
# build (sub_make_done=1, exported by the kernel's top Makefile) goes to gcc —
# compiles, probes, --version alike — and with --sccache only kernel compiles
# go through sccache. Everything else (tools/lkl's user-space half) goes to
# zig-cc. Uses stub compilers.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
wrapper="$root/scripts/lib/lkl-linux-cc.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

printf '#!/bin/sh\necho "sccache $*"\n' > "$tmp/sccache"
printf '#!/bin/sh\necho "gcc $*"\n' > "$tmp/gcc"
printf '#!/bin/sh\necho "zig $*"\n' > "$tmp/zig-cc"
chmod +x "$tmp/sccache" "$tmp/gcc" "$tmp/zig-cc"

check() {  # <want> <sub_make_done value, or - for unset> [--sccache] args...
    local want="$1" smd="$2" got
    shift 2
    if [[ "$smd" == - ]]; then
        got="$(env -u sub_make_done PATH="$tmp:$PATH" "$wrapper" "$@")"
    else
        got="$(sub_make_done="$smd" PATH="$tmp:$PATH" "$wrapper" "$@")"
    fi
    [[ "$got" == "$want" ]] || { echo "FAIL: smd=$smd $* -> '$got' (want '$want')"; exit 1; }
}
g="$tmp/gcc" z="$tmp/zig-cc"

# Inside the kernel build: gcc for everything.
check "gcc -D__KERNEL__ -c a.c -o a.o"           1 "$g" "$z" -D__KERNEL__ -c a.c -o a.o
check "gcc --version"                             1 "$g" "$z" --version
check "gcc -Werror -c -x c /dev/null -o t.o"      1 "$g" "$z" -Werror -c -x c /dev/null -o t.o
check "gcc -print-file-name=include"              1 "$g" "$z" -print-file-name=include
# --sccache: kernel compiles only; probes stay on plain gcc (sccache can't
# cache -o /dev/null).
check "sccache $g -D__KERNEL__ -c a.c -o a.o"     1 --sccache "$g" "$z" -D__KERNEL__ -c a.c -o a.o
check "gcc -Werror -c -x c /dev/null -o /dev/null" 1 --sccache "$g" "$z" -Werror -c -x c /dev/null -o /dev/null
# Outside it: zig.
check "zig -c lib/posix-host.c -o p.o"            - "$g" "$z" -c lib/posix-host.c -o p.o
check "zig -shared -o liblkl.so x.o"              - --sccache "$g" "$z" -shared -o liblkl.so x.o
check "zig -c a.c"                                0 "$g" "$z" -c a.c

echo "OK: lkl-linux-cc.sh routes the kernel build to gcc and tools/lkl to zig"
