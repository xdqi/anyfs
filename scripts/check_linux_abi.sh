#!/usr/bin/env bash
# Gate for linux-amd64 artifacts. Fails if an ELF file needs a newer glibc
# than the floor, links a shared library outside the allowlist, or carries an
# undefined dynamic symbol with no version.
#
# Usage: check_linux_abi.sh [--allow-undefined=ERE] <max-glibc> <file|dir>...
#   <max-glibc>  2.11 for the CLI tarball, 2.25 for code loaded into Electron.
#   --allow-undefined=ERE
#                unversioned undefined symbols matching ERE are expected, e.g.
#                an addon's napi_* imports, which the host process provides.
#   Directories are searched recursively; files that aren't ELF are skipped.
#
# Why the third check: a function the floor's glibc lacks fails an executable
# link, but a -shared link silently leaves it undefined with no version. The
# version scan never sees it, and the library only fails to load on an older
# system. Needs binutils >= 2.35 (nm -D prints symbol versions).
set -euo pipefail

# glibc's own libraries, plus the two the tarball bundles.
ALLOWED_NEEDED=(
    libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2
    ld-linux-x86-64.so.2
    liblkl.so libanyfs-qemublk.so
)

usage() {
    awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0" >&2
    exit 2
}

allow_undef=""
case "${1:-}" in
    --allow-undefined=*) allow_undef="${1#*=}"; shift ;;
esac
[[ $# -ge 2 ]] || usage
max="$1"
shift

# ver_gt A B: version A is newer than B.
ver_gt() { [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" == "$1" ]]; }

is_elf() { [[ "$(head -c4 "$1" | od -An -tx1 | tr -d ' \n')" == 7f454c46 ]]; }

fail=0
checked=0

check_file() {
    local f="$1" top="" v lib sym ok
    local -a bad=()
    checked=$((checked + 1))

    # Highest GLIBC_x.y in the version-needs section (.gnu.version_r).
    while read -r v; do
        if [[ "$v" == PRIVATE ]]; then
            bad+=("uses GLIBC_PRIVATE")
            continue
        fi
        if [[ -z "$top" ]] || ver_gt "$v" "$top"; then top="$v"; fi
    done < <(readelf -V --wide "$f" | sed -n '/Version needs section/,$p' \
                 | grep -oE 'Name: GLIBC_[0-9A-Z_.]+' | sed 's/^Name: GLIBC_//')
    if [[ -n "$top" ]] && ver_gt "$top" "$max"; then
        bad+=("needs GLIBC_$top > $max:$(objdump -T "$f" | awk -v v="GLIBC_$top" '$0 ~ v {printf " %s", $NF}')")
    fi

    while read -r lib; do
        ok=0
        for v in "${ALLOWED_NEEDED[@]}"; do [[ "$lib" == "$v" ]] && ok=1; done
        [[ $ok -eq 1 ]] || bad+=("NEEDED $lib is not glibc or bundled")
    done < <(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')

    while read -r sym; do
        [[ -n "$allow_undef" && "$sym" =~ $allow_undef ]] && continue
        bad+=("undefined symbol $sym has no version (missing at the floor?)")
    done < <(nm -D --undefined-only "$f" | awk '$1 == "U" && $2 !~ /@/ {print $2}')

    if [[ ${#bad[@]} -gt 0 ]]; then
        printf 'FAIL %s\n' "$f"
        printf '       %s\n' "${bad[@]}"
        fail=1
    else
        printf 'ok   %s (GLIBC_%s)\n' "$f" "${top:-none}"
    fi
}

for arg in "$@"; do
    while IFS= read -r -d '' f; do
        if is_elf "$f"; then check_file "$f"; fi
    done < <(find "$arg" -type f -print0)
done

if [[ $checked -eq 0 ]]; then
    echo "check_linux_abi: no ELF files in: $*" >&2
    exit 1
fi
[[ $fail -eq 0 ]] && echo "check_linux_abi: $checked ELF file(s) within GLIBC_$max"
exit "$fail"
