#!/usr/bin/env bash
# Gate for scripts/lib/sccache-cc.sh: compiles whose output is /dev/null
# bypass sccache; everything else goes through it. Uses stub `sccache` and
# compiler scripts, so no real sccache is needed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
wrapper="$root/scripts/lib/sccache-cc.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/sccache" <<'EOF'
#!/bin/sh
echo "via-sccache $*"
EOF
cat > "$tmp/fakecc" <<'EOF'
#!/bin/sh
echo "direct $*"
EOF
chmod +x "$tmp/sccache" "$tmp/fakecc"

check() {
    local want="$1"
    shift
    local got
    got="$(PATH="$tmp:$PATH" "$wrapper" "$tmp/fakecc" "$@")"
    [[ "$got" == "$want "* ]] || { echo "FAIL: $* -> '$got' (want $want)"; exit 1; }
}

# Kconfig's scripts/as-version.sh probe.
check direct -Wa,--version -c -x assembler-with-cpp /dev/null -o /dev/null
check direct -c foo.c -o/dev/null
check via-sccache -c foo.c -o foo.o
check via-sccache --version
# /dev/null as an *input* is still cacheable.
check via-sccache -c -x c /dev/null -o probe.o

echo "OK: sccache-cc.sh bypasses sccache only for -o /dev/null"
