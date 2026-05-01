#!/bin/bash
# Benchmark with dm-delay (simulates high-latency storage)
# Usage: sudo ./tests/bench_delay.sh [delay_ms] [iterations]
set -e

DELAY_MS=${1:-50}
ITERS=${2:-10}
IMG="/tmp/anyfs_bench_delay.img"
DM_NAME="anyfs_delay_$$"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH="${SCRIPT_DIR}/../builddir/bench_backends"
BENCH_CONC="${SCRIPT_DIR}/../builddir/bench_concurrent"

cleanup() {
    sudo dmsetup remove "$DM_NAME" 2>/dev/null || true
    [ -n "$LOOP" ] && sudo losetup -d "$LOOP" 2>/dev/null || true
    rm -f "$IMG"
}
trap cleanup EXIT

# Create test image
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
mkfs.ext4 -q -F "$IMG"
MNTDIR=$(mktemp -d)
mount "$IMG" "$MNTDIR"
echo "Hello from delayed disk" > "$MNTDIR/hello.txt"
umount "$MNTDIR"
rmdir "$MNTDIR"

# Setup dm-delay
LOOP=$(losetup --find --show "$IMG")
SECTORS=$(blockdev --getsz "$LOOP")
dmsetup create "$DM_NAME" --table "0 $SECTORS delay $LOOP 0 $DELAY_MS"
DELAYED_DEV="/dev/mapper/$DM_NAME"

echo "============================================================"
echo " Benchmark with ${DELAY_MS}ms read delay (dm-delay)"
echo " Device: $DELAYED_DEV (loop=$LOOP)"
echo " Iterations: $ITERS"
echo "============================================================"
echo ""

# Sequential benchmark
echo "--- Sequential benchmark ---"
"$BENCH" "$DELAYED_DEV" ext4 0 "$ITERS" 2>&1 | grep -v "^\[" | grep -v "^$"
echo ""

# Concurrent benchmark
echo "--- Concurrent benchmark (4 threads x $ITERS reads) ---"
"$BENCH_CONC" "$DELAYED_DEV" ext4 0 4 "$ITERS" 2>&1 | grep -v "^\[" | grep -v "^$"

echo ""
echo "Done."
