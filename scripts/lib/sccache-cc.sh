#!/bin/sh
# scripts/lib/sccache-cc.sh — compiler launcher: `sccache <compiler> ARGS…`,
# except a compile whose output is /dev/null, which runs `<compiler> ARGS…`
# directly.
#
# sccache serves a cache hit by writing a temp file in the output's directory
# and renaming it over the output — for `-o /dev/null` that is /dev, so the
# hit fails with EACCES and prints nothing. Kbuild's assembler probe
# (scripts/as-version.sh: `$(CC) -Wa,--version -c … /dev/null -o /dev/null`)
# is such a compile: once CI's persistent sccache cache held its result,
# every later run failed with "Sorry, this assembler is not supported".
#
# Usage: CC="scripts/lib/sccache-cc.sh <compiler>"   (<compiler>: one word)
compiler=$1
shift
prev=
for a in "$@"; do
    if [ "$a" = -o/dev/null ] || { [ "$prev" = -o ] && [ "$a" = /dev/null ]; }; then
        exec "$compiler" "$@"
    fi
    prev=$a
done
exec sccache "$compiler" "$@"
