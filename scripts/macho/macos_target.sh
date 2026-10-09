# macOS deployment targets for the Mach-O build, sourced by every macOS build
# script (the kernel dylib, the host library, the dependency sysroot, QEMU,
# anyfs and the Node addon) and by the <arch>-macos-cc launchers.
#
#   arm64   11.0   the first macOS on Apple Silicon, so the arm64 floor.
#   x86_64  10.13  GLib (scripts/lib/sysroot_sources.sh), which QEMU and
#                  anyfs link, refuses to configure for anything older. The
#                  host library alone would run on 10.12, the first macOS with
#                  clock_gettime(CLOCK_MONOTONIC). It goes below 11.0 only
#                  because patch 08 (patches/linux/macho/) replaces
#                  preadv()/pwritev(), which need 11.0, with a pread()/pwrite()
#                  loop when the deployment target is older.
#
# The host builds compile with -Werror=unguarded-availability, so a call to
# anything newer than these fails at compile time.
# shellcheck shell=bash

MACOS_MIN_arm64=11.0
MACOS_MIN_x86_64=10.13

# Both functions exit on an unknown arch. Assign their output to a variable
# (v="$(macos_min "$arch")") rather than using it inline in a command's
# arguments, where set -e would not see the failure.

# macos_min ARCH: print the deployment target for ARCH (arm64 | x86_64).
macos_min() {
    case "$1" in
        arm64)  echo "$MACOS_MIN_arm64" ;;
        x86_64) echo "$MACOS_MIN_x86_64" ;;
        *)      echo "macos_target: unknown arch '$1'" >&2; exit 1 ;;
    esac
}

# macos_zig_target ARCH: print zig's -target for ARCH, e.g.
# x86_64-macos.10.13.0. zig wants all three version components.
macos_zig_target() {
    local min
    min="$(macos_min "$1")" || exit 1
    case "$1" in
        arm64)  echo "aarch64-macos.$min.0" ;;
        x86_64) echo "x86_64-macos.$min.0" ;;
    esac
}
