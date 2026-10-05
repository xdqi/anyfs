#!/bin/sh
# scripts/lib/lkl-mingw-cc.sh — CC for a mingw LKL build through sccache.
#
# The mingw LKL build uses two compilers. LKL's Makefile puts tools/lkl/bin
# first on PATH for the kernel build, where `x86_64-w64-mingw32-gcc` is a
# shim that sends kernel code to cygwin-gcc (LP64: the kernel needs 64-bit
# long) and everything else to the real mingw gcc (LLP64). sccache must see
# the real compiler, by absolute path, to hash it and ship the right
# toolchain to dist workers — so handing it the shim, or an absolute mingw
# gcc that skips the shim, is wrong.
#
# This launcher sends kernel compiles (`-D__KERNEL__ … -c`), the bulk of the
# build, to `sccache <cygwin-gcc>`. Every other call (links, `-S`, user-space
# compiles, probes) goes to <mingw-gcc-name> through PATH exactly as without
# a CC override: the shim inside the kernel build, the real mingw gcc
# outside it.
#
# Usage: CC="scripts/lib/lkl-mingw-cc.sh <abs cygwin gcc> <mingw gcc name>"
cygwin_cc=$1
mingw_cc=$2
shift 2
kernel=
compile=
for a in "$@"; do
    case "$a" in
    -D__KERNEL__) kernel=1 ;;
    -c) compile=1 ;;
    esac
done
if [ -n "$kernel" ] && [ -n "$compile" ]; then
    exec sccache "$cygwin_cc" "$@"
fi
exec "$mingw_cc" "$@"
