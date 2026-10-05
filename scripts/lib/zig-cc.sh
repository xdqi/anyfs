#!/bin/sh
# scripts/lib/zig-cc.sh — body of the zig-cc / zig-c++ compiler wrappers that
# build every linux-amd64 artifact. Build systems get the two launchers next
# to this file (scripts/lib/zig-cc, scripts/lib/zig-c++), never this script.
#
# Environment:
#   ANYFS_ZIG_TARGET   zig target; default x86_64-linux-gnu.2.11, the glibc
#                      floor of the CLI tools and libraries. Code loaded into
#                      Electron uses x86_64-linux-gnu.2.25. An explicit target
#                      also pins the baseline x86-64 CPU, whatever the host.
#   ANYFS_ZIG          zig binary; default <repo>/.toolchain/zig/zig, the link
#                      scripts/lib/config.sh keeps pointed at toolchains.zig.
#   ANYFS_ZIG_SCCACHE  1 = compile through `sccache <zig> cc|c++ …`. The
#                      sccache fork recognises zig only by an executable stem
#                      of `zig` with argv1 cc/c++, so it must get zig itself,
#                      never this wrapper. The target stays a visible
#                      argument, so the cache is partitioned by target.
#
# Where zig cc differs from the gcc that build systems expect:
#   -dumpmachine        zig prints its versioned target and then rejects it;
#                       meson and autoconf need a plain GNU triple.
#   -print-search-dirs  zig reports the HOST gcc's library dirs, and meson's
#                       find_library() then links /usr/lib/x86_64-linux-gnu
#                       libraries. Report none: libraries come from -L and
#                       pkg-config only.
#   -Wp,-v              gcc's include-dir listing (LKL's Makefile.autoconf
#                       find_include); zig rejects it, clang's -v prints the
#                       same " <dir>" lines.
#   -pie with -shared   gcc lets -shared win (LKL's Makefile.conf puts -pie on
#                       every link); zig refuses the combination. Drop -pie.
#   -UNDEBUG            zig defines NDEBUG at -O1 and above.
#   -fno-sanitize=undefined  zig enables UBSan at -O0.
#   -g0                 zig emits DWARF even without -g. Added only when the
#                       caller asks for no debug info: zig doesn't let a later
#                       plain -g undo -g0.
# These go before the caller's flags, so an explicit -DNDEBUG or -fsanitize
# still wins.
mode=$1
shift
target=${ANYFS_ZIG_TARGET:-x86_64-linux-gnu.2.11}
zig=${ANYFS_ZIG:-$(cd "$(dirname "$0")/../.." && pwd)/.toolchain/zig/zig}

shared=
nodebug=-g0
for a in "$@"; do
    case $a in
    -dumpmachine)
        case $target in
        *-linux-gnu*) echo "${target%%-*}-unknown-linux-gnu" ;;
        *) echo "${target%%.*}" ;;
        esac
        exit 0
        ;;
    -print-search-dirs | --print-search-dirs)
        printf 'install: \nprograms: =\nlibraries: =\n'
        exit 0
        ;;
    -shared) shared=1 ;;
    -g0) ;;
    -g*) nodebug= ;;
    esac
done
# Rewrite in place: each argument is shifted off the front and re-appended.
for a in "$@"; do
    shift
    case $a in
    -Wp,-v) a=-v ;;
    -pie) [ -n "$shared" ] && continue ;;
    esac
    set -- "$@" "$a"
done

if [ "${ANYFS_ZIG_SCCACHE:-0}" = 1 ]; then
    exec sccache "$zig" "$mode" -target "$target" \
        -UNDEBUG -fno-sanitize=undefined $nodebug "$@"
fi
exec "$zig" "$mode" -target "$target" -UNDEBUG -fno-sanitize=undefined $nodebug "$@"
