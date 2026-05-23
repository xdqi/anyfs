#!/bin/bash
# Integration smoke test: anyfs-lspart / anyfs-ksmbd / anyfs-nfsd against a
# Debian 13 generic-cloud qcow2 (or any cloud image with an ext4 root on p1
# and an ESP/vfat on p15).
#
# This is the scripted form of the manual verification used during the
# CI bring-up: each binary must light up against a known-good qcow2
# (proving the QEMU block backend, ksmbd-tools wiring, and host_proxy
# data path all work end-to-end).
#
# Usage:
#   tests/smoke-debian-qcow2.sh [--build-dir=DIR] [--nfs-mount] <qcow2>
#
#   --build-dir=DIR   directory containing built anyfs-* binaries
#                     (default: ./build-anyfs-linux-amd64)
#   --nfs-mount       also `sudo mount -t nfs4` the NFS share and assert
#                     contents (default: just confirm daemon comes up)
#
# Required:    smbclient (samba-client) in PATH.
# Optional:    nfs-common + sudo for --nfs-mount.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$SRC_DIR/build-anyfs-linux-amd64"
WITH_NFS_MOUNT=0
QCOW=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir=*) BUILD_DIR="${1#--build-dir=}"; shift ;;
        --build-dir)   BUILD_DIR="$2"; shift 2 ;;
        --nfs-mount)   WITH_NFS_MOUNT=1; shift ;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -*) echo "Unknown option: $1" >&2; exit 2 ;;
        *)  QCOW="$1"; shift ;;
    esac
done

if [[ -z "$QCOW" ]]; then
    echo "ERROR: missing qcow2 path" >&2
    echo "Usage: $0 [--build-dir=DIR] [--nfs-mount] <qcow2>" >&2
    exit 2
fi
if [[ ! -f "$QCOW" ]]; then
    echo "ERROR: $QCOW does not exist" >&2
    exit 2
fi

LSPART="$BUILD_DIR/src/lspart/anyfs-lspart"
KSMBD="$BUILD_DIR/anyfs-ksmbd"
NFSD="$BUILD_DIR/anyfs-nfsd"
for b in "$LSPART" "$KSMBD" "$NFSD"; do
    if [[ ! -x "$b" ]]; then
        echo "ERROR: $b not found or not executable" >&2
        echo "       Build first: scripts/build_anyfs.sh --components=core,server" >&2
        exit 2
    fi
done

# Pick high, unlikely-to-collide ports (avoids the 4455 / 20049 defaults
# in case another anyfs process is already running on the same host).
SMB_PORT=44555
NFS_PORT=22049

PASS=0
FAIL=0
KSMBD_PID=""
NFSD_PID=""
MOUNT_DIR=""

log_pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
log_fail() { echo "  [FAIL] $1: $2"; FAIL=$((FAIL+1)); }

cleanup() {
    set +e
    if [[ -n "$MOUNT_DIR" ]]; then
        sudo umount "$MOUNT_DIR" 2>/dev/null
        rmdir "$MOUNT_DIR" 2>/dev/null
    fi
    if [[ -n "$KSMBD_PID" ]] && kill -0 "$KSMBD_PID" 2>/dev/null; then
        kill -TERM "$KSMBD_PID" 2>/dev/null
        wait "$KSMBD_PID" 2>/dev/null
    fi
    if [[ -n "$NFSD_PID" ]] && kill -0 "$NFSD_PID" 2>/dev/null; then
        kill -TERM "$NFSD_PID" 2>/dev/null
        wait "$NFSD_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

echo "================================================================"
echo " anyfs-reader smoke test — Debian qcow2"
echo "   qcow2:     $QCOW"
echo "   build:     $BUILD_DIR"
echo "   smb port:  $SMB_PORT"
echo "   nfs port:  $NFS_PORT"
echo "   nfs mount: $([[ $WITH_NFS_MOUNT -eq 1 ]] && echo yes || echo no)"
echo "================================================================"

# ── 1. anyfs-lspart ────────────────────────────────────────────────────
echo
echo "--- [1/3] anyfs-lspart ---"
LSPART_OUT="$("$LSPART" "$QCOW" 2>&1)"
echo "$LSPART_OUT"
if echo "$LSPART_OUT" | grep -qiE "disk0/p1[[:space:]].*ext4"; then
    log_pass "lspart: p1 reported as ext4"
else
    log_fail "lspart: p1 ext4" "no 'disk0/p1 ... ext4' line in output"
fi
if echo "$LSPART_OUT" | grep -qiE "disk0/p15[[:space:]].*(vfat|fat)"; then
    log_pass "lspart: p15 reported as vfat/FAT"
else
    log_fail "lspart: p15 vfat" "no 'disk0/p15 ... vfat' line in output"
fi

# ── 2. anyfs-ksmbd + smbclient ─────────────────────────────────────────
echo
echo "--- [2/3] anyfs-ksmbd + smbclient ---"
KSMBD_LOG="$(mktemp)"
"$KSMBD" "$QCOW" --share root=disk0/p1 -P "$SMB_PORT" \
    > "$KSMBD_LOG" 2>&1 &
KSMBD_PID=$!

# Wait up to 30 s for SMB to listen.
for i in $(seq 1 60); do
    if ss -tlnp 2>/dev/null | grep -q ":$SMB_PORT\b"; then
        break
    fi
    if ! kill -0 "$KSMBD_PID" 2>/dev/null; then
        echo "ERROR: anyfs-ksmbd died before listening:"
        cat "$KSMBD_LOG"
        log_fail "ksmbd: process died" "see log above"
        break
    fi
    sleep 0.5
done

if kill -0 "$KSMBD_PID" 2>/dev/null; then
    SMB_OUT="$(smbclient -N "//127.0.0.1/root" --port="$SMB_PORT" -c 'ls' 2>&1 || true)"
    echo "$SMB_OUT"
    # Sentinel rootfs entries. We match by name only — Debian's usr-merge
    # makes /sbin /bin /lib /lib64 symlinks, which smbclient reports with
    # flag 'r' (regular) instead of 'D' (directory); a strict type check
    # would falsely fail.
    SMB_OK=1
    for name in etc usr sbin; do
        if ! echo "$SMB_OUT" | grep -qE "^[[:space:]]+$name[[:space:]]"; then
            SMB_OK=0
            log_fail "ksmbd: smbclient ls" "expected '$name' in listing"
        fi
    done
    [[ $SMB_OK -eq 1 ]] && log_pass "ksmbd: smbclient ls shows Debian rootfs"
fi

kill -TERM "$KSMBD_PID" 2>/dev/null || true
wait "$KSMBD_PID" 2>/dev/null || true
KSMBD_PID=""
rm -f "$KSMBD_LOG"

# ── 3. anyfs-nfsd ──────────────────────────────────────────────────────
echo
echo "--- [3/3] anyfs-nfsd ---"
NFSD_LOG="$(mktemp)"
"$NFSD" "$QCOW" --share root=disk0/p1 -P "$NFS_PORT" \
    > "$NFSD_LOG" 2>&1 &
NFSD_PID=$!

# Wait for the "NFSv4 server ready" log line OR for the listen socket.
NFSD_READY=0
for i in $(seq 1 60); do
    if grep -q "NFSv4 server ready" "$NFSD_LOG" 2>/dev/null; then
        NFSD_READY=1
        break
    fi
    if ss -tlnp 2>/dev/null | grep -q ":$NFS_PORT\b"; then
        NFSD_READY=1
        break
    fi
    if ! kill -0 "$NFSD_PID" 2>/dev/null; then
        echo "ERROR: anyfs-nfsd died before listening:"
        cat "$NFSD_LOG"
        log_fail "nfsd: process died" "see log above"
        break
    fi
    sleep 0.5
done

if [[ $NFSD_READY -eq 1 ]]; then
    log_pass "nfsd: server ready on port $NFS_PORT"

    if [[ $WITH_NFS_MOUNT -eq 1 ]]; then
        # Settle delay: the "ready" log line fires when the LKL socket is
        # bound; the NFSv4 export advertisement / mountd cache may still
        # be initializing for another moment. A short pause avoids racing
        # the first PUTROOTFH compound (which appeared to mount cleanly
        # but return an empty listing in the first CI run).
        sleep 2

        MOUNT_DIR="$(mktemp -d /tmp/anyfs-nfs.XXXXXX)"
        echo "  mount: 127.0.0.1:/root -> $MOUNT_DIR (port=$NFS_PORT)"
        MOUNT_OUT_FILE="$(mktemp)"
        # Drop `soft` so a wedged server gives a clearer failure than a
        # silent empty listing; raise verbosity so failure modes (auth,
        # path, version) are visible in the CI log.
        if sudo mount -t nfs4 -v "127.0.0.1:/root" "$MOUNT_DIR" \
                -o "port=$NFS_PORT,vers=4,timeo=50,retrans=2" \
                > "$MOUNT_OUT_FILE" 2>&1; then
            echo "  mount stdout/stderr:"
            sed 's/^/    /' "$MOUNT_OUT_FILE"

            echo "  ls -la $MOUNT_DIR:"
            LS_OUT="$(sudo ls -la "$MOUNT_DIR" 2>&1 || true)"
            echo "$LS_OUT" | sed 's/^/    /'

            MOUNT_OUT="$(sudo ls -1 "$MOUNT_DIR" 2>&1 || true)"
            MNT_OK=1
            for name in etc usr sbin; do
                if ! echo "$MOUNT_OUT" | grep -qx "$name"; then
                    MNT_OK=0
                    log_fail "nfsd: nfs4 mount ls" "expected '$name' in listing"
                fi
            done
            [[ $MNT_OK -eq 1 ]] && log_pass "nfsd: nfs4 mount shows Debian rootfs"

            sudo umount "$MOUNT_DIR" 2>/dev/null
        else
            echo "  mount FAILED. stdout/stderr:"
            sed 's/^/    /' "$MOUNT_OUT_FILE"
            log_fail "nfsd: nfs4 mount" "sudo mount returned non-zero"
        fi
        rm -f "$MOUNT_OUT_FILE"
        rmdir "$MOUNT_DIR" 2>/dev/null
        MOUNT_DIR=""

        # On any nfsd failure (mount didn't list / mount errored), the
        # daemon log is the most useful single artifact — print it
        # before we kill the daemon, regardless of pass/fail.
        echo "  --- nfsd log (tail) ---"
        tail -n 80 "$NFSD_LOG" | sed 's/^/    /'
    fi
else
    echo "--- nfsd log ---"; cat "$NFSD_LOG"
fi

kill -TERM "$NFSD_PID" 2>/dev/null || true
wait "$NFSD_PID" 2>/dev/null || true
NFSD_PID=""
rm -f "$NFSD_LOG"

# ── Summary ────────────────────────────────────────────────────────────
echo
echo "================================================================"
echo " RESULTS: $PASS passed, $FAIL failed"
echo "================================================================"
[[ $FAIL -eq 0 ]]
