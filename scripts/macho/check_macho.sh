#!/bin/bash
# Gate for the macOS build outputs, run on Linux.
#
# Usage: check_macho.sh --arch=arm64|x86_64 [--dylib=NAME]... [--rpath=PATH]...
#                       [--allow-undefined=REGEX] FILE...
#
# FILE is a static archive or a linked image (executable, dylib, .node
# bundle). Every Mach-O object in it must be for ARCH and built for ARCH's
# deployment target in macos_target.sh. Images must also:
#   - load only libSystem, the system frameworks curl uses
#     (scripts/macho/sdk-stubs), and the extra dylibs named with --dylib
#     (e.g. @rpath/liblkl-kernel.dylib);
#   - carry exactly the LC_RPATH entries given with --rpath, so no build
#     directory leaks into a shipped binary;
#   - bind every undefined symbol to one of those dylibs (two-level
#     namespace). Symbols matching --allow-undefined (a .node bundle's
#     napi_*) may instead be looked up dynamically in the host process;
#   - import nothing weakly, except what the SDK headers check for NULL
#     themselves: a weak import is how the linker binds a function newer
#     than the deployment target, and calling it on an older macOS jumps to
#     NULL (see zig-macos.sh);
#   - on arm64, be signed (ad-hoc is enough): arm64 macOS maps no unsigned
#     code. zig signs arm64 links by itself; x86_64 macOS runs unsigned code.
# NM / OTOOL / OBJDUMP override the LLVM 20/19 tools (llvm_tools.sh).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=macos_target.sh
source "$HERE/macos_target.sh"
# shellcheck source=llvm_tools.sh
source "$HERE/llvm_tools.sh"

NM="${NM:-$(llvm_tool llvm-nm)}" || exit 1
OTOOL="${OTOOL:-$(llvm_tool llvm-otool)}" || exit 1
OBJDUMP="${OBJDUMP:-$(llvm_tool llvm-objdump)}" || exit 1

die() { echo "check_macho: $*" >&2; exit 1; }

arch="" dylibs=() rpaths=() allow_undef=""
files=()
for a in "$@"; do
    case "$a" in
        --arch=*)            arch="${a#--arch=}" ;;
        --dylib=*)           dylibs+=("${a#--dylib=}") ;;
        --rpath=*)           rpaths+=("${a#--rpath=}") ;;
        --allow-undefined=*) allow_undef="${a#--allow-undefined=}" ;;
        -h|--help)           awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
        -*)                  die "unknown option $a" ;;
        *)                   files+=("$a") ;;
    esac
done
[[ ${#files[@]} -gt 0 ]] || die "no files"
case "$arch" in
    arm64)  cputype=ARM64 ;;
    x86_64) cputype=X86_64 ;;
    *)      die "--arch=arm64|x86_64 is required" ;;
esac
min="$(macos_min "$arch")"
for t in "$NM" "$OTOOL" "$OBJDUMP"; do
    command -v "$t" >/dev/null || die "$t not found"
done

# Load commands every image may carry beyond --dylib.
# Weak imports the SDK headers guard themselves (FD_SET checks the pointer).
weak_ok=(___darwin_check_fd_set_overflow)

system_dylibs=(
    /usr/lib/libSystem.B.dylib
    /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
    /System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration
)

fail=0
bad() { echo "check_macho: $1: $2" >&2; fail=1; }

# arch_and_min FILE: every Mach-O header in FILE (all archive members) has
# cputype $cputype, and every build-version / version-min load command says $min.
arch_and_min() {
    local f="$1" hdr types mins
    hdr="$("$OBJDUMP" --macho --private-headers "$f" 2>/dev/null)" || { bad "$f" "not Mach-O"; return; }
    types="$(awk '/^ *(0x)?MH_MAGIC/ {print $2}' <<<"$hdr" | sort -u)"
    [[ -n $types ]] || { bad "$f" "no Mach-O objects"; return; }
    [[ $types == "$cputype" ]] || bad "$f" "cputype {${types//$'\n'/ }}, want $cputype"
    # LC_BUILD_VERSION prints "minos X", LC_VERSION_MIN_MACOSX "version X".
    mins="$(awk '$1=="cmd" {cmd=$2} cmd=="LC_BUILD_VERSION" && $1=="minos" {print $2} cmd=="LC_VERSION_MIN_MACOSX" && $1=="version" {print $2}' <<<"$hdr" | sort -u)"
    [[ -n $mins ]] || { bad "$f" "no deployment target load command"; return; }
    [[ $mins == "$min" ]] || bad "$f" "deployment target {${mins//$'\n'/ }}, want $min"
}

check_image() {
    local f="$1" d ok r want got lc
    lc="$("$OTOOL" -l "$f")"
    # Dependent dylibs (LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB).
    while read -r d; do
        [[ -n $d ]] || continue
        ok=0
        for want in "${system_dylibs[@]}" "${dylibs[@]}"; do
            [[ $d == "$want" ]] && ok=1
        done
        [[ $ok == 1 ]] || bad "$f" "loads $d"
    done < <(awk '$1=="cmd" {cmd=$2} (cmd ~ /^LC_(LOAD|LOAD_WEAK|REEXPORT|LAZY_LOAD)_DYLIB$/) && $1=="name" {print $2}' <<<"$lc")
    got="$(awk '$1=="cmd" {cmd=$2} cmd=="LC_RPATH" && $1=="path" {print $2}' <<<"$lc" | sort)"
    want="$(printf '%s\n' "${rpaths[@]}" | sed '/^$/d' | sort)"
    [[ $got == "$want" ]] || bad "$f" "rpaths {${got//$'\n'/ }}, want {${want//$'\n'/ }}"
    [[ $arch != arm64 ]] || grep -q 'cmd LC_CODE_SIGNATURE' <<<"$lc" || bad "$f" "not signed"
    # Undefined symbols: with two-level namespace nm -m names the dylib
    # ("(from libSystem)"); a flat lookup prints "dynamically looked up".
    while read -r r; do
        local sym="${r##* }"
        [[ -n $allow_undef && $sym =~ $allow_undef ]] && continue
        bad "$f" "undefined $sym is not bound to a dylib"
    done < <("$NM" -m -u "$f" | grep -v '(from ' || true)
    local w
    while read -r w; do
        [[ " ${weak_ok[*]} " == *" $w "* ]] || bad "$f" "weak import $w (newer than macOS $min?)"
    done < <("$NM" -m -u "$f" | awk '/weak external/ {print $4}')
}

for f in "${files[@]}"; do
    [[ -f $f ]] || { bad "$f" "missing"; continue; }
    arch_and_min "$f"
    [[ "$(head -c 7 "$f" | tr -d '\0')" == '!<arch>' ]] || check_image "$f"
done
[[ $fail == 0 ]] || exit 1
echo "check_macho: ${#files[@]} file(s) OK ($arch, macOS $min)"
