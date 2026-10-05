#!/bin/sh
# scripts/lib/lkl-linux-cc.sh — CC for the linux-amd64 LKL build.
#
# The kernel half (freestanding, -nostdinc) stays on the host gcc, ISA pinned
# by KCFLAGS in build_lkl.sh; tools/lkl's user-space half (posix-host.c, the
# liblkl.so link, tests) uses zig at the glibc floor. Every call made from
# inside the kernel build must reach gcc, not just compiles: Kconfig's
# cc-version.sh, `$(CC) --version` and cc-option probes would otherwise
# record zig's clang as the kernel compiler.
#
# The signal is sub_make_done=1: the kernel's top Makefile exports it before
# it recurses into the O= dir, so every process of the kernel build inherits
# it (recipes, sub-makes, parse-time $(shell), make 4.3 and 4.4), while
# tools/lkl's own make never sets it. build_lkl.sh always builds with O= and
# clears any stray value first.
#
# --sccache sends kernel compiles (-D__KERNEL__ … -c) through sccache, the
# bulk of the build; kernel probes stay on plain gcc (sccache mishandles
# -o /dev/null). zig-cc does its own sccache hand-off (ANYFS_ZIG_SCCACHE).
#
# Usage: CC="scripts/lib/lkl-linux-cc.sh [--sccache] <abs gcc> <abs zig-cc>"
sccache=
if [ "$1" = --sccache ]; then
    sccache=1
    shift
fi
gcc=$1
zigcc=$2
shift 2

if [ "${sub_make_done-}" != 1 ]; then
    exec "$zigcc" "$@"
fi
if [ -n "$sccache" ]; then
    kernel=
    compile=
    for a in "$@"; do
        case $a in
        -D__KERNEL__) kernel=1 ;;
        -c) compile=1 ;;
        esac
    done
    if [ -n "$kernel" ] && [ -n "$compile" ]; then
        exec sccache "$gcc" "$@"
    fi
fi
exec "$gcc" "$@"
