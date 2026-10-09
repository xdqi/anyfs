#!/bin/bash
# Body of the macOS compiler launchers next to this file:
#
#   arm64-macos-cc   arm64-macos-c++   x86_64-macos-cc   x86_64-macos-c++
#
# Each is a symlink to this script and runs scripts/lib/zig-cc.sh for the
# arch's deployment target in macos_target.sh. The target lives in the
# launcher's name, not in the environment, so meson and QEMU's configure can
# record the launcher as the compiler and a later ninja-triggered regen still
# compiles for the same target.
#
# -Werror=unguarded-availability: a call to an API newer than the deployment
# target is an error unless it sits under __builtin_available. Without it the
# linker silently makes such a function a weak import, which is NULL on an
# older macOS: QEMU's configure found preadv() (macOS 11) and strchrnul()
# (15.4) and would have crashed on older systems. With it, configure and
# meson probes fail for those functions and the projects use their own
# fallbacks; scripts/macho/check_macho.sh refuses weak imports.
name=${0##*/}
arch=${name%-macos-*}
mode=${name##*-macos-}
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# shellcheck source=macos_target.sh
source "$here/macos_target.sh"
case $mode in
cc | c++) ;;
*) echo "zig-macos.sh: run through <arch>-macos-cc or <arch>-macos-c++, not $name" >&2; exit 1 ;;
esac
ANYFS_ZIG_TARGET="$(macos_zig_target "$arch")" || exit 1
export ANYFS_ZIG_TARGET
exec "$here/../lib/zig-cc.sh" "$mode" -Werror=unguarded-availability "$@"
