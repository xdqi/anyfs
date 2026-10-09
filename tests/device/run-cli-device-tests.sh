#!/bin/bash
# CLI tests against a read-only virtual block device: the fixture parts.img
# (tests/macos/make-fixtures.sh) presented by the OS as a disk, e.g.
#   Linux    losetup --read-only --partscan          /dev/loopN, /dev/loopNpK
#   macOS    hdiutil attach -readonly -nomount       /dev/diskN, /dev/diskNsK, /dev/rdiskN
#   Windows  Mount-DiskImage -Access ReadOnly (VHDX) \\.\PhysicalDriveN
# Attaching, permission changes and detaching belong to the caller (the CI
# workflows' scripts/ci/test-device.{sh,ps1}); this script only reads the
# devices named in their state file. It never touches any other disk.
#
# Usage: run-cli-device-tests.sh --tools=DIR --device-env=FILE
#            [--reference=EXPECTED_JSON] [--logdir=DIR] [--no-servers]
#            [--expect-denied]
#
#   --tools        directory with anyfs-lspart, anyfs-ksmbd, anyfs-nfsd
#                  (.exe on Windows; run from Git Bash there)
#   --device-env   shell file setting ANYFS_TEST_DEVICE (whole disk),
#                  ANYFS_TEST_RAW_DEVICE (macOS /dev/rdiskN, else empty) and
#                  ANYFS_TEST_PART_DEVICES (partition nodes in table order,
#                  space-separated, may be empty)
#   --reference    default tests/macos/reference/expected.json; the lspart
#                  table is the lspart/parts.img.txt next to it
#   --no-servers   only anyfs-lspart
#   --expect-denied  the caller has removed this user's access to the
#                  devices: every tool must fail cleanly (exit 1, a
#                  "Permission denied" / "Access is denied" message, no
#                  crash, no hang) instead of reading
#
# Checks, for the whole disk (and on macOS the raw /dev/rdiskN node) with
# both backends (ANYFS_BACKEND=raw and the default, QEMU where it can open
# devices): anyfs-lspart prints the reference table; anyfs-ksmbd and
# anyfs-nfsd serve partition 3 (ext4 "fixroot") and every reference file read
# through a protocol client has the reference size and SHA-256; the servers
# stop on SIGINT with exit 0. For each partition node: anyfs-lspart prints
# that partition as a whole-disk row, and the fixroot node is served as
# --share disk0. Protocol clients: Linux smbclient and an NFSv4 mount (sudo),
# macOS mount_smbfs and an NFSv4 mount (sudo), Windows the Python
# smbprotocol package (tests/device/smb_get.py); Windows has no NFSv4 client,
# so NFS is skipped there, and a Windows server cannot be sent Ctrl-C from
# here, so its clean shutdown is skipped too (it is killed instead).
# Prints ok/FAIL/skip lines; exit 0 when nothing failed.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
tools="" devenv="" reference="$repo/tests/macos/reference/expected.json"
logdir="" servers=1 expect_denied=0
for a in "$@"; do
    case "$a" in
        --tools=*) tools="${a#--tools=}" ;;
        --device-env=*) devenv="${a#--device-env=}" ;;
        --reference=*) reference="${a#--reference=}" ;;
        --logdir=*) logdir="${a#--logdir=}" ;;
        --no-servers) servers=0 ;;
        --expect-denied) expect_denied=1 ;;
        -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
[ -n "$tools" ] && [ -n "$devenv" ] || { echo "--tools and --device-env are required" >&2; exit 2; }
ANYFS_TEST_DEVICE="" ANYFS_TEST_RAW_DEVICE="" ANYFS_TEST_PART_DEVICES=""
# shellcheck disable=SC1090
. "$devenv"
[ -n "$ANYFS_TEST_DEVICE" ] || { echo "$devenv sets no ANYFS_TEST_DEVICE" >&2; exit 2; }
lspart_ref="$(dirname "$reference")/lspart/parts.img.txt"
logdir="${logdir:-${TMPDIR:-/tmp}/anyfs-device-tests-$$}"
mkdir -p "$logdir"

case "$(uname -s)" in
    Linux) os=linux exe="" ;;
    Darwin) os=macos exe="" ;;
    MINGW*|MSYS*|CYGWIN*) os=windows exe=".exe" ;;
    *) echo "unsupported OS $(uname -s)" >&2; exit 2 ;;
esac
py="$(command -v python3 || command -v python)"
[ -n "$py" ] || { echo "python3 is required (reference parsing)" >&2; exit 2; }

npass=0 nfail=0 nskip=0
ok()   { npass=$((npass + 1)); echo "ok   $*"; }
bad()  { nfail=$((nfail + 1)); echo "FAIL $*"; }
skip() { nskip=$((nskip + 1)); echo "skip $*"; }
sha() {
    if command -v sha256sum > /dev/null; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}
# A log/file name from a device path.
tag() { echo "$1" | tr -c 'A-Za-z0-9\n' '_'; }
# used BACKEND LOG: the backend that actually served (the QEMU backend logs
# "[qemu_blk] open"), e.g. "default=raw" on macOS, where QEMU can't open disks.
used() {
    local b=qemu
    grep -q '^\[qemu_blk\] open' "$2" 2>/dev/null || b=raw
    if [ "$1" = default ]; then echo "default=$b"; else echo "$b"; fi
}

# Reference files of parts.img: "<partition> <path> <size> <sha256>" lines.
refs="$logdir/reference-files.txt"
"$py" - "$reference" > "$refs" <<'PY'
import json, sys
for img in json.load(open(sys.argv[1]))["images"]:
    if img["file"] == "parts.img":
        for f in img["files"]:
            print(img["enter"], f["path"], f["size"], f["sha256"])
PY
[ -s "$refs" ] || { echo "no parts.img files in $reference" >&2; exit 2; }
fs_part="$(awk 'NR == 1 {print $1}' "$refs")"

# run_tool BACKEND LOG CMD...: run with ANYFS_BACKEND=BACKEND ("default"
# leaves it unset); stdout to LOG.out, stderr to LOG.err.
run_tool() {
    local backend=$1 log=$2
    shift 2
    if [ "$backend" = default ]; then
        (unset ANYFS_BACKEND; "$@") > "$log.out" 2> "$log.err"
    else
        ANYFS_BACKEND=$backend "$@" > "$log.out" 2> "$log.err"
    fi
}

# Expected anyfs-lspart output for partition node K: the reference row of
# disk0/pK as the whole-disk row disk0, or only the header when that
# partition has no filesystem.
part_ref() {
    awk -v k="disk0/p$1" 'NR == 1 {print; next}
        $1 == k { if ($5 != "?") { sub(/^disk0\/p[0-9]+ */, ""); printf "%-18s %s\n", "disk0", $0 } }' "$lspart_ref"
}

lspart_check() { # lspart_check DEVICE BACKEND EXPECTED_FILE WHAT
    local dev=$1 backend=$2 want=$3 what=$4 log
    log="$logdir/lspart-$(tag "$dev")-$backend"
    run_tool "$backend" "$log" "$tools/anyfs-lspart$exe" "$dev"
    local rc=$?
    tr -d '\r' < "$log.out" > "$log.txt"
    if [ $rc -ne 0 ]; then
        bad "lspart $dev ($what, backend $backend): exit $rc: $(grep -v '^\[' "$log.err" | tail -1)"
    elif diff -u "$want" "$log.txt" > "$log.diff"; then
        ok "lspart $dev ($what, backend $(used "$backend" "$log.err")): $(($(wc -l < "$log.txt") - 1)) row(s) match the reference"
    else
        bad "lspart $dev ($what, backend $backend): differs from the reference ($log.diff)"
    fi
}

wait_for() { # wait_for FILE PATTERN SECONDS PID
    local i=0
    while [ $i -lt $(($3 * 10)) ]; do
        grep -q "$2" "$1" 2>/dev/null && return 0
        kill -0 "$4" 2>/dev/null || return 1
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

stop_server() { # stop_server PID LOG WHAT
    local pid=$1 log=$2 what=$3 i=0
    if [ $os = windows ]; then
        taskkill //F //T //PID "$pid" > /dev/null 2>&1 || kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        skip "$what: clean shutdown (a Windows server can't be sent Ctrl-C from here; killed)"
        return
    fi
    kill -INT "$pid" 2>/dev/null
    while kill -0 "$pid" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null
        bad "$what: still running 30 s after SIGINT (killed)"
        return
    fi
    wait "$pid"
    local rc=$?
    if [ $rc -eq 0 ]; then ok "$what: clean shutdown"; else bad "$what: exit $rc after SIGINT ($log)"; fi
}

# check_files DIR WHAT: every reference file under DIR has its size and hash.
check_files() {
    local dir=$1 what=$2 path size want got
    while read -r _ path size want; do
        got="$(sha "$dir/$path" 2>/dev/null)"
        if [ "$got" = "$want" ]; then
            ok "$what: /$path ($size bytes) sha256 matches"
        else
            bad "$what: /$path sha256 ${got:-missing}, want $want"
        fi
    done < "$refs"
}

# smb_fetch PORT DIR: copy the reference files from share "anyfs" into DIR.
smb_fetch() {
    local port=$1 dir=$2 path size want
    case $os in
        linux)
            while read -r _ path size want; do
                mkdir -p "$dir/$(dirname "$path")"
                smbclient -N -p "$port" //127.0.0.1/anyfs -c "get \"$path\" \"$dir/$path\"" \
                    >> "$dir.client.log" 2>&1 || return 1
            done < "$refs" ;;
        macos)
            local mnt="$dir.mnt"
            mkdir -p "$mnt"
            mount_smbfs -N "//guest@127.0.0.1:$port/anyfs" "$mnt" >> "$dir.client.log" 2>&1 \
                || mount_smbfs "//guest:guest@127.0.0.1:$port/anyfs" "$mnt" >> "$dir.client.log" 2>&1 \
                || return 1
            while read -r _ path size want; do
                mkdir -p "$dir/$(dirname "$path")"
                cp "$mnt/$path" "$dir/$path" 2>> "$dir.client.log"
            done < "$refs"
            umount "$mnt" >> "$dir.client.log" 2>&1 ;;
        windows)
            local args=()
            while read -r _ path size want; do
                mkdir -p "$dir/$(dirname "$path")"
                args+=("$path" "$(cygpath -w "$dir/$path")")
            done < "$refs"
            "$py" "$here/smb_get.py" 127.0.0.1 "$port" anyfs "${args[@]}" >> "$dir.client.log" 2>&1 ;;
    esac
}

# nfs_fetch PORT DIR: mount the NFSv4 export read-only and copy the files.
nfs_fetch() {
    local port=$1 dir=$2 mnt="$2.mnt" path size want
    mkdir -p "$mnt"
    case $os in
        linux) sudo mount -t nfs4 -o "port=$port,ro" 127.0.0.1:/ "$mnt" ;;
        macos) sudo mount -t nfs -o "vers=4,port=$port,ro,nobrowse" 127.0.0.1:/ "$mnt" ;;
    esac >> "$dir.client.log" 2>&1 || return 1
    while read -r _ path size want; do
        mkdir -p "$dir/$(dirname "$path")"
        cp "$mnt/$path" "$dir/$path" 2>> "$dir.client.log"
    done < "$refs"
    # shellcheck disable=SC2024  # the log is ours; only umount runs as root
    sudo umount "$mnt" >> "$dir.client.log" 2>&1
}

port=14480
# serve KIND DEVICE SHARE BACKEND: start anyfs-ksmbd/anyfs-nfsd on DEVICE,
# read the files through a client, stop it.
serve() {
    local kind=$1 dev=$2 share=$3 backend=$4 what log dir pid ready
    port=$((port + 1))
    what="$kind $dev --share $share (backend $backend)"
    log="$logdir/$kind-$(tag "$dev")-$backend"
    dir="$log.files"
    rm -rf "$dir" && mkdir -p "$dir"
    if [ $kind = nfsd ] && [ $os = windows ]; then
        skip "$what: no NFSv4 client on Windows (its NFS client is v2/v3 only)"
        return
    fi
    if [ $kind = ksmbd ]; then
        ready="SMB server ready"
        set -- "$tools/anyfs-ksmbd$exe" "$dev" --share "anyfs=$share" -P "$port"
    else
        ready="NFSv4 server ready"
        set -- "$tools/anyfs-nfsd$exe" "$dev" --share "$share" -P "$port"
    fi
    if [ "$backend" = default ]; then
        (unset ANYFS_BACKEND; exec "$@") > "$log.log" 2>&1 &
    else
        ANYFS_BACKEND=$backend "$@" > "$log.log" 2>&1 &
    fi
    pid=$!
    if ! wait_for "$log.log" "$ready" 120 "$pid"; then
        bad "$what: not ready ($log.log: $(grep -iE 'error|fail' "$log.log" | tail -1))"
        stop_server "$pid" "$log.log" "$what"
        return
    fi
    ok "$what: server ready on port $port, backend $(used "$backend" "$log.log")"
    if [ $kind = ksmbd ]; then smb_fetch "$port" "$dir"; else nfs_fetch "$port" "$dir"; fi
    if [ $? -eq 0 ]; then
        check_files "$dir" "$what"
    else
        bad "$what: client failed ($dir.client.log)"
    fi
    stop_server "$pid" "$log.log" "$what"
}

# denied DEVICE: each tool fails cleanly without access.
denied() {
    local dev=$1 log rc pid i
    log="$logdir/denied-lspart-$(tag "$dev")"
    run_tool default "$log" "$tools/anyfs-lspart$exe" "$dev"
    rc=$?
    if [ $rc -eq 1 ] && grep -qiE 'permission denied|access is denied|operation not permitted' "$log.err"; then
        ok "lspart $dev without access: exit 1, $(grep -iE 'denied|not permitted' "$log.err" | tail -1)"
    else
        bad "lspart $dev without access: exit $rc, expected 1 and a permission error ($log.err)"
    fi
    [ $servers -eq 1 ] || return
    for kind in ksmbd nfsd; do
        log="$logdir/denied-$kind-$(tag "$dev")"
        port=$((port + 1))
        "$tools/anyfs-$kind$exe" "$dev" --share "disk0/p$fs_part" -P "$port" > "$log.log" 2>&1 &
        pid=$!
        i=0
        while kill -0 "$pid" 2>/dev/null && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null
            bad "$kind $dev without access: still running after 60 s"
            continue
        fi
        wait "$pid"
        rc=$?
        if [ $rc -ne 0 ] && [ $rc -lt 128 ] && grep -qiE 'permission denied|access is denied|operation not permitted' "$log.log"; then
            ok "$kind $dev without access: exit $rc with a permission error"
        else
            bad "$kind $dev without access: exit $rc ($log.log)"
        fi
    done
}

echo "=== anyfs CLI device tests ($os, $(date))"
echo "device $ANYFS_TEST_DEVICE${ANYFS_TEST_RAW_DEVICE:+, raw $ANYFS_TEST_RAW_DEVICE}${ANYFS_TEST_PART_DEVICES:+, partitions $ANYFS_TEST_PART_DEVICES}"
echo "tools $tools; logs $logdir"

if [ $expect_denied -eq 1 ]; then
    for dev in $ANYFS_TEST_DEVICE $ANYFS_TEST_RAW_DEVICE; do denied "$dev"; done
else
    for dev in $ANYFS_TEST_DEVICE $ANYFS_TEST_RAW_DEVICE; do
        for backend in raw default; do
            lspart_check "$dev" "$backend" "$lspart_ref" "whole disk"
        done
    done
    k=0
    for dev in $ANYFS_TEST_PART_DEVICES; do
        k=$((k + 1))
        part_ref "$k" > "$logdir/part$k.ref"
        for backend in raw default; do
            lspart_check "$dev" "$backend" "$logdir/part$k.ref" "partition $k node"
        done
    done
    if [ $servers -eq 1 ]; then
        for backend in raw default; do
            serve ksmbd "$ANYFS_TEST_DEVICE" "disk0/p$fs_part" "$backend"
            serve nfsd "$ANYFS_TEST_DEVICE" "disk0/p$fs_part" "$backend"
        done
        [ -z "$ANYFS_TEST_RAW_DEVICE" ] || serve ksmbd "$ANYFS_TEST_RAW_DEVICE" "disk0/p$fs_part" default
        k=0
        for dev in $ANYFS_TEST_PART_DEVICES; do
            k=$((k + 1))
            [ "$k" = "$fs_part" ] || continue
            serve ksmbd "$dev" disk0 default
            serve nfsd "$dev" disk0 default
        done
    fi
fi

echo
if [ $nfail -eq 0 ]; then verdict=PASS; else verdict=FAIL; fi
echo "$verdict ($npass ok, $nfail failed, $nskip skipped) — logs in $logdir"
[ $nfail -eq 0 ]
