#!/usr/bin/env bash
# Windows CI only (Git Bash): run a command with a restricted Basic User
# token (runas /trustlevel:0x20000, no Administrators group), the way an
# unprivileged user would. The CI account is an administrator, so this is
# how the "access denied" side of the device tests is exercised there.
# runas starts the command asynchronously: wait for its exit code.
#
# Usage: run-restricted-win.sh <log> -- <command> [args...]
# Exits with the command's exit code; its output is in <log>.
set -euo pipefail

log="${1:?usage: run-restricted-win.sh <log> -- <command> [args...]}"
shift
[[ "${1:-}" == -- ]] && shift
[[ $# -gt 0 ]] || { echo "run-restricted-win: no command" >&2; exit 2; }

dir="$(mktemp -d)"
rc_file="$dir/rc"
wrapper="$dir/run.cmd"
win() { cygpath -w "$1"; }
bash_exe="$(win "$(command -v bash)")"
# Everything runs through bash -c so that the arguments survive cmd.exe.
# runas gives the command a fresh environment and cwd: carry over both.
printf -v cmd '%q ' "$@"
{
    printf 'cd %q\n' "$PWD"
    printf 'export PATH=%q\n' "$PATH"
    printf '%s\n' "$cmd"
} > "$dir/cmd.sh"
{
    printf '@echo off\r\n'
    printf '"%s" "%s" > "%s" 2>&1\r\n' "$bash_exe" "$(win "$dir/cmd.sh")" "$(win "$log")"
    printf 'echo %%ERRORLEVEL%% > "%s"\r\n' "$(win "$rc_file")"
} > "$wrapper"
MSYS_NO_PATHCONV=1 runas /trustlevel:0x20000 "cmd /c \"$(win "$wrapper")\""
for _ in $(seq 1 600); do [[ -s "$rc_file" ]] && break; sleep 1; done
[[ -s "$rc_file" ]] || { echo "run-restricted-win: no exit code after 600 s" >&2; exit 1; }
rc="$(tr -dc 0-9 < "$rc_file")"
cat "$log"
exit "${rc:-1}"
