# macOS deployment targets for the Mach-O build, sourced by
# build_kernel_dylib.sh, build_host_lib.sh, build_smoke.sh and
# test_elf2dylib.sh.
#
#   arm64   11.0   the first macOS on Apple Silicon, so the arm64 floor.
#   x86_64  10.12  the host library calls clock_gettime(CLOCK_MONOTONIC),
#                  which macOS 10.12 introduced. It goes below 11.0 only
#                  because patch 08 (patches/linux/macho/) replaces
#                  preadv()/pwritev(), which need 11.0, with a pread()/pwrite()
#                  loop when the deployment target is older.
#
# The host builds compile with -Werror=unguarded-availability, so a call to
# anything newer than these fails at compile time.
# shellcheck shell=bash

MACOS_MIN_arm64=11.0
MACOS_MIN_x86_64=10.12

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
# x86_64-macos.10.12.0. zig wants all three version components.
macos_zig_target() {
    local min
    min="$(macos_min "$1")" || exit 1
    case "$1" in
        arm64)  echo "aarch64-macos.$min.0" ;;
        x86_64) echo "x86_64-macos.$min.0" ;;
    esac
}
