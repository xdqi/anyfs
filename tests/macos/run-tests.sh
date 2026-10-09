#!/bin/bash
# macOS runtime tests for anyfs-reader, run on a Mac from the test bundle
# that scripts/macho/package_macos_tests.sh builds (layout below). Needs only
# what macOS ships: bash 3.2, shasum, mount_smbfs, mount_nfs, codesign.
#
# Usage: tests/run-tests.sh [--app PATH.app] [--node PATH] [--skip-nfs] [--loops N]
#
#   --app      a packaged electron-demo .app with the addon staged
#              (stage-native-macos.sh): runs the addon test with Electron as
#              node, from the app's Resources/native, and the Electron
#              main-process smoke (ANYFS_NATIVE_SMOKE)
#   --node     a node binary for the addon test against native/ when no app
#              is given (default: node on PATH, else the test is skipped)
#   --skip-nfs NFS needs a root mount (sudo prompts); skip it
#   --loops    passes of the addon test (default 3)
#
# Bundle layout (the script finds everything relative to itself):
#   bin/      anyfs-lspart anyfs-ksmbd anyfs-nfsd + core test programs
#   lib/      liblkl-kernel.dylib (bin/ finds it via @loader_path/../lib)
#   native/   anyfs_native.node + liblkl-kernel.dylib
#   fixtures/ parts.img parts-zlib.dmg parts-bz2.dmg [ubuntu-26.10.qcow2]
#   tests/    this script, native-smoke.mjs, expected.json, expected-files.txt,
#             lspart/<fixture>.txt
#
# Every check prints "ok"/"FAIL"/"skip"; the summary ends in PASS or FAIL and
# the exit status is 0 only for PASS. Logs go to $LOGDIR (default
# ~/anyfs-macos-test-logs/<date>); send that directory back with the output.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
bin="$root/bin"
fix="$root/fixtures"
app="" node_bin="" skip_nfs=0 loops=3
while [ $# -gt 0 ]; do
    case "$1" in
        --app) app="$2"; shift ;;
        --node) node_bin="$2"; shift ;;
        --skip-nfs) skip_nfs=1 ;;
        --loops) loops="$2"; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

LOGDIR="${LOGDIR:-$HOME/anyfs-macos-test-logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOGDIR"
npass=0 nfail=0 nskip=0
ok()   { npass=$((npass + 1)); echo "ok   $*"; }
bad()  { nfail=$((nfail + 1)); echo "FAIL $*"; }
skip() { nskip=$((nskip + 1)); echo "skip $*"; }
check() { # check DESCRIPTION CMD...: ok when CMD succeeds
    local what="$1"; shift
    if "$@"; then ok "$what"; else bad "$what"; fi
}
sha() { shasum -a 256 "$1" | awk '{print $1}'; }

# kill_tree PID: a child and what it started (sudo-started ones via sudo -n).
kill_tree() {
    pkill -KILL -P "$1" 2>/dev/null || sudo -n pkill -KILL -P "$1" 2>/dev/null
    kill -KILL "$1" 2>/dev/null || sudo -n kill -KILL "$1" 2>/dev/null
}
# gone PID SECONDS: wait until a child has ended, at most SECONDS; 1 if not.
gone() {
    local i=0
    while kill -0 "$1" 2>/dev/null; do
        [ $i -ge $(($2 * 10)) ] && return 1
        sleep 0.1
        i=$((i + 1))
    done
}
# bounded SECONDS CMD...: run CMD, killing it (and what it started) when it
# overruns; its status, or 124 after a timeout (a FAIL line says so). No
# step may stall the run: CI then cancels the whole job without logs.
bounded() {
    local limit=$1 pid
    shift
    "$@" &
    pid=$!
    if ! gone "$pid" "$limit"; then
        kill_tree "$pid"
        gone "$pid" 10 && wait "$pid" 2>/dev/null
        echo "FAIL timed out after $limit s (killed): $*" >&2
        return 124
    fi
    wait "$pid"
}
# Mounts live outside $LOGDIR: a stuck one there would also hang whoever
# copies or uploads the logs.
MNTDIR="$(mktemp -d "${TMPDIR:-/tmp}/anyfs-mnt.XXXXXX")"
servers=""
cleanup() { # what a cancelled or failed run leaves: servers, mounts
    local p m
    for p in $servers; do kill -0 "$p" 2>/dev/null && kill_tree "$p"; done
    for m in "$MNTDIR"/*; do
        [ -d "$m" ] || continue
        # Not a mount point any more: umount fails, harmlessly. (mount(8)
        # shows $TMPDIR as /private/var/..., so don't grep for it.)
        umount -f "$m" 2>/dev/null || sudo -n umount -f "$m" 2>/dev/null
        rmdir "$m" 2>/dev/null
    done
    rmdir "$MNTDIR" 2>/dev/null
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# wait_for FILE PATTERN SECONDS: until PATTERN appears in FILE.
wait_for() {
    local i=0
    while [ $i -lt $(($3 * 10)) ]; do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# stop_server PID LOG NAME: SIGINT, then the exit status within 30 s.
stop_server() {
    local pid=$1 log=$2 name=$3
    servers=${servers/ $pid / }
    kill -INT "$pid" 2>/dev/null
    if ! gone "$pid" 30; then
        bad "$name: still running 30 s after SIGINT (killed)"
        kill_tree "$pid"
        return
    fi
    wait "$pid"
    local rc=$?
    check "$name: clean shutdown (exit $rc)" [ "$rc" -eq 0 ]
}

echo "=== anyfs-reader macOS tests ($(date))"
echo "host: $(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null) ($(sw_vers -buildVersion 2>/dev/null)), $(uname -m)"
echo "bundle: $root"
echo "logs: $LOGDIR"
{ sw_vers; uname -a; } > "$LOGDIR/host.txt" 2>&1

# --- 0. the binaries load ------------------------------------------------------
bundle_arch="$(cat "$root/ARCH" 2>/dev/null)"
host_arch="$(uname -m)"
if [ "$bundle_arch" = "x86_64" ] && [ "$host_arch" = "arm64" ]; then
    echo "note: x86_64 bundle on Apple Silicon runs under Rosetta 2"
elif [ -n "$bundle_arch" ] && [ "$bundle_arch" != "$host_arch" ]; then
    bad "bundle is $bundle_arch, this Mac is $host_arch"
fi
for f in "$bin"/anyfs-lspart "$root"/lib/liblkl-kernel.dylib "$root"/native/anyfs_native.node; do
    [ -f "$f" ] || continue
    if [ "$bundle_arch" = arm64 ]; then
        check "codesign -v $(basename "$f")" bounded 60 codesign -v "$f"
    fi
done
bounded 60 "$bin/anyfs-lspart" --help > "$LOGDIR/lspart-help.txt" 2>&1
check "anyfs-lspart starts (dyld finds liblkl-kernel.dylib)" grep -q '^Usage:' "$LOGDIR/lspart-help.txt"

# --- 1. core tests (the meson unit suite's programs) --------------------------
run_core() { # run_core NAME ARGS...: exit 0 = ok, 77 = skip
    local name=$1; shift
    [ -x "$bin/$name" ] || { skip "$name (not in bundle)"; return; }
    (cd "$LOGDIR" && bounded 300 "$bin/$name" "$@") > "$LOGDIR/$name${1:+-$1}.log" 2>&1
    local rc=$?
    case $rc in
        0)  ok "$name $*" ;;
        124) bad "$name $* (timed out after 300 s, $LOGDIR/$name${1:+-$1}.log)" ;;
        77) skip "$name $* (needs a Linux-only tool, see its log)" ;;
        *)  bad "$name $* (exit $rc, $LOGDIR/$name${1:+-$1}.log)" ;;
    esac
}
for t in test_u8 test_path_dsl test_mount_opts test_name_escape test_legacy \
         test_share_helpers test_tls_ca test_session_reopen test_session_whole_part; do
    run_core "$t"
done
run_core test_qemu_thread basic
ANYFS_QEMU_TIMEOUT_MS=2000 run_core test_qemu_thread watchdog

# --- 2. anyfs-lspart: partition table, types, fstype, label, uuid --------------
for img in parts.img parts-zlib.dmg parts-bz2.dmg ubuntu-26.10.qcow2; do
    [ -f "$fix/$img" ] || { skip "lspart $img (fixture not in bundle)"; continue; }
    for run in 1 2; do
        bounded 120 "$bin/anyfs-lspart" "$fix/$img" > "$LOGDIR/lspart-$img-$run.txt" 2> "$LOGDIR/lspart-$img-$run.err"
        rc=$?
        if [ $rc -ne 0 ]; then bad "lspart $img run $run: exit $rc"; continue; fi
        if diff -u "$here/lspart/$img.txt" "$LOGDIR/lspart-$img-$run.txt" > "$LOGDIR/lspart-$img-$run.diff"; then
            ok "lspart $img run $run: table matches the Linux reference"
        else
            bad "lspart $img run $run: differs from the Linux reference ($LOGDIR/lspart-$img-$run.diff)"
        fi
    done
done

# expected-files.txt lines: <fixture> <partition> <path> <size> <sha256>
files_for() { awk -v img="$1" '$1 == img' "$here/expected-files.txt"; }

# --- 3. anyfs-ksmbd: start, mount_smbfs as guest, read, unmount, stop -----------
smb_test() { # smb_test FIXTURE PARTITION PORT
    local img=$1 part=$2 port=$3 log="$LOGDIR/ksmbd-$1.log" mnt="$MNTDIR/smb-$1"
    [ -f "$fix/$img" ] || { skip "ksmbd $img (fixture not in bundle)"; return; }
    mkdir -p "$mnt"
    "$bin/anyfs-ksmbd" "$fix/$img" --share "anyfs=disk0/p$part" -P "$port" > "$log" 2>&1 &
    local pid=$!
    servers="$servers $pid "
    if ! wait_for "$log" "SMB server ready" 120; then
        bad "ksmbd $img: no 'SMB server ready' within 120 s ($log)"
        stop_server $pid "$log" "ksmbd $img"
        return
    fi
    ok "ksmbd $img: server ready on port $port"
    if bounded 60 mount_smbfs -N "//guest@127.0.0.1:$port/anyfs" "$mnt" > "$LOGDIR/mount_smbfs-$img.log" 2>&1 \
       || bounded 60 mount_smbfs "//guest:guest@127.0.0.1:$port/anyfs" "$mnt" >> "$LOGDIR/mount_smbfs-$img.log" 2>&1; then
        ok "ksmbd $img: mount_smbfs //guest@127.0.0.1:$port/anyfs"
        bounded 60 ls -la "$mnt" > "$LOGDIR/smb-ls-$img.txt" 2>&1
        local _img _part path size want got
        while read -r _img _part path size want; do
            [ "$_part" = "$part" ] || continue
            got="$(bounded 300 sha "$mnt/$path" 2>> "$LOGDIR/mount_smbfs-$img.log")"
            check "ksmbd $img: /$path over SMB ($size bytes) sha256 matches" [ "$got" = "$want" ]
        done <<EOF
$(files_for "$img")
EOF
        bounded 300 cp "$mnt/$(files_for "$img" | awk -v p="$part" '$2 == p {print $3; exit}')" "$LOGDIR/smb-copy-$img.bin" 2>/dev/null
        if bounded 60 umount "$mnt"; then
            ok "ksmbd $img: umount"
        else
            bad "ksmbd $img: umount"
            umount -f "$mnt" 2>/dev/null
        fi
    else
        bad "ksmbd $img: mount_smbfs failed ($LOGDIR/mount_smbfs-$img.log); try Finder > Go > Connect to Server > smb://guest@127.0.0.1:$port/anyfs"
    fi
    stop_server $pid "$log" "ksmbd $img"
}
smb_test parts.img 3 14455
smb_test ubuntu-26.10.qcow2 1 14456

# --- 4. anyfs-nfsd: start, mount_nfs (NFSv4, needs sudo), read, unmount, stop ---
nfs_test() { # nfs_test FIXTURE PARTITION PORT
    local img=$1 part=$2 port=$3 log="$LOGDIR/nfsd-$1.log" mnt="$MNTDIR/nfs-$1"
    [ -f "$fix/$img" ] || { skip "nfsd $img (fixture not in bundle)"; return; }
    if [ $skip_nfs -eq 1 ]; then skip "nfsd $img (--skip-nfs)"; return; fi
    mkdir -p "$mnt"
    "$bin/anyfs-nfsd" "$fix/$img" --share "disk0/p$part" -P "$port" > "$log" 2>&1 &
    local pid=$!
    servers="$servers $pid "
    if ! wait_for "$log" "NFSv4 server ready" 120; then
        bad "nfsd $img: not ready within 120 s ($log)"
        stop_server $pid "$log" "nfsd $img"
        return
    fi
    ok "nfsd $img: server up on port $port"
    echo "nfsd: mounting needs root; sudo may ask for your password"
    # soft: a stalled server fails reads instead of hanging them; soft + ro
    # alone means local locks, which macOS refuses for NFSv4, hence "locks".
    # shellcheck disable=SC2024  # the log is the user's; only mount runs as root
    if bounded 150 sudo mount -t nfs -o "vers=4,port=$port,ro,nobrowse,soft,locks" "127.0.0.1:/" "$mnt" > "$LOGDIR/mount_nfs-$img.log" 2>&1; then
        ok "nfsd $img: mount -t nfs -o vers=4,port=$port 127.0.0.1:/"
        bounded 60 ls -la "$mnt" > "$LOGDIR/nfs-ls-$img.txt" 2>&1
        local _img _part path size want got
        while read -r _img _part path size want; do
            [ "$_part" = "$part" ] || continue
            got="$(bounded 300 sha "$mnt/$path" 2>> "$LOGDIR/mount_nfs-$img.log")"
            check "nfsd $img: /$path over NFS ($size bytes) sha256 matches" [ "$got" = "$want" ]
        done <<EOF
$(files_for "$img")
EOF
        if bounded 60 sudo umount "$mnt"; then
            ok "nfsd $img: umount"
        else
            bad "nfsd $img: umount"
            sudo umount -f "$mnt" 2>/dev/null
        fi
    else
        bad "nfsd $img: mount failed ($LOGDIR/mount_nfs-$img.log)"
    fi
    stop_server $pid "$log" "nfsd $img"
}
nfs_test parts.img 3 20049
nfs_test ubuntu-26.10.qcow2 1 20050

# --- 4b. anyfs-fuse through macFUSE (an external dependency) ------------------
fuse_test() { # fuse_test FIXTURE PARTITION
    local img=$1 part=$2 log="$LOGDIR/fuse-$1.log" mnt="$MNTDIR/fuse-$1"
    [ -f "$fix/$img" ] || { skip "fuse $img (fixture not in bundle)"; return; }
    mkdir -p "$mnt"
    "$bin/anyfs-fuse" -f -o "ro,part=$part" "$fix/$img" "$mnt" > "$log" 2>&1 &
    local pid=$! i=0
    while [ $i -lt 600 ] && [ -z "$(ls -A "$mnt" 2>/dev/null)" ] && kill -0 $pid 2>/dev/null; do
        sleep 0.1; i=$((i + 1))
    done
    if [ -z "$(ls -A "$mnt" 2>/dev/null)" ]; then
        bad "fuse $img: mount did not appear within 60 s ($log; on first use macOS may ask to allow the macFUSE system extension)"
        umount "$mnt" 2>/dev/null
        stop_server $pid "$log" "fuse $img"
        return
    fi
    ok "fuse $img: mounted p$part at $mnt"
    local _img _part path size want got
    while read -r _img _part path size want; do
        [ "$_part" = "$part" ] || continue
        got="$(sha "$mnt/$path" 2>/dev/null)"
        check "fuse $img: /$path over FUSE ($size bytes) sha256 matches" [ "$got" = "$want" ]
    done <<EOF
$(files_for "$img")
EOF
    check "fuse $img: umount" umount "$mnt"
    i=0
    while kill -0 $pid 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
    if kill -0 $pid 2>/dev/null; then
        bad "fuse $img: anyfs-fuse still running 30 s after umount"
        kill -KILL $pid 2>/dev/null
    else
        wait $pid
        local rc=$?
        check "fuse $img: anyfs-fuse exits after umount (exit $rc)" [ $rc -eq 0 ]
    fi
}
if [ ! -x "$bin/anyfs-fuse" ]; then
    skip "anyfs-fuse (not in this bundle)"
elif [ ! -f /usr/local/lib/libfuse3.4.dylib ]; then
    skip "anyfs-fuse: macFUSE is not installed (external dependency, https://macfuse.github.io)"
else
    fuse_test parts.img 3
    fuse_test ubuntu-26.10.qcow2 1
fi

# --- 5. the Node addon: boot, metadata, mount, list, extract + hash, repeat ----
addon_runner="" addon=""
if [ -n "$app" ]; then
    exe="$app/Contents/MacOS/$(basename "$app" .app)"
    addon="$app/Contents/Resources/native/anyfs_native.node"
    addon_runner="env ELECTRON_RUN_AS_NODE=1 $exe"
elif [ -n "$node_bin" ] || command -v node > /dev/null 2>&1; then
    addon="$root/native/anyfs_native.node"
    addon_runner="${node_bin:-node}"
fi
if [ -z "$addon_runner" ]; then
    skip "addon test (no --app and no node on PATH)"
else
    # shellcheck disable=SC2086  # addon_runner is a command line
    bounded 900 $addon_runner "$here/native-smoke.mjs" --addon "$addon" --fixtures "$fix" \
        --expected "$here/expected.json" --loops "$loops" > "$LOGDIR/native-smoke.log" 2>&1
    rc=$?
    grep -E '^(ok|FAIL|skip|PASS|native-smoke)' "$LOGDIR/native-smoke.log" | sed 's/^/    /' | tail -12
    check "addon test via $(basename "${addon_runner##* }") ($(tail -1 "$LOGDIR/native-smoke.log"))" [ $rc -eq 0 ]
fi

# --- 6. the Electron main process loads the addon (native-loader.ts) ----------
if [ -n "$app" ]; then
    # The app's headless smoke (electron-demo src/native-smoke.ts): mount the
    # partition labelled fixroot, read hello.txt, halt; JSON report in $out.
    out="$LOGDIR/electron-native-smoke.json"
    rm -f "$out"
    ANYFS_NATIVE_SMOKE=1 ANYFS_NATIVE_IMAGE="$fix/parts.img" ANYFS_NATIVE_PART=fixroot \
        ANYFS_NATIVE_READ=hello.txt ANYFS_NATIVE_OUT="$out" \
        bounded 300 "$exe" > "$LOGDIR/electron-native-smoke.log" 2>&1
    rc=$?
    check "Electron main process: native smoke exit $rc" [ $rc -eq 0 ]
    check "Electron main process: report ok" grep -Eq '"ok": *true' "$out"
    want="$(files_for parts.img | awk '$3 == "hello.txt" {print $5}')"
    check "Electron main process: hello.txt sha256 matches" grep -q "\"sha256\": *\"$want\"" "$out"
else
    skip "Electron main-process smoke (no --app)"
fi

echo
if [ $nfail -eq 0 ]; then verdict=PASS; else verdict=FAIL; fi
echo "$verdict ($npass ok, $nfail failed, $nskip skipped) — logs in $LOGDIR"
echo "$verdict ($npass ok, $nfail failed, $nskip skipped)" > "$LOGDIR/summary.txt"
[ $nfail -eq 0 ]
