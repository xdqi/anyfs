#!/bin/bash
# AnyFS-Reader integration test
# Tests: ext4, xfs, btrfs, vfat (single-partition & GPT mixed-partition)
# Runs each test with BOTH raw (pread) and GIO backends.
# LKL kernel output is NOT suppressed — shows full boot + mount logs.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_BIN="${SCRIPT_DIR}/../builddir/test_raw_mount"
TMPDIR="/tmp/anyfs-test-$$"
PASS=0
FAIL=0

cleanup() {
    sudo umount "$TMPDIR/mnt" 2>/dev/null || true
    if [[ -n "$LOOPDEV" ]]; then
        sudo losetup -d "$LOOPDEV" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/mnt"

log_pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
log_fail() { echo "  [FAIL] $1: $2"; FAIL=$((FAIL+1)); }

# Determine available backends
BACKENDS=("raw")
if "$TEST_BIN" --gio /dev/null ext4 0 2>&1 | grep -q "GIO backend not compiled"; then
    echo "NOTE: GIO backend not compiled, testing raw only."
else
    BACKENDS+=("gio")
fi
echo "Backends to test: ${BACKENDS[*]}"
echo ""

run_test() {
    local img="$1" fstype="$2" part="$3" expect_file="$4" label="$5" backend="$6"
    local backend_flag=""
    [[ "$backend" == "gio" ]] && backend_flag="--gio"

    local full_label="[$backend] $label"
    echo ""
    echo "--- Running: $full_label ---"
    echo "    Image: $img | FS: $fstype | Partition: $part | Backend: $backend"
    echo ""

    OUTPUT=$("$TEST_BIN" $backend_flag "$img" "$fstype" "$part" 2>&1)
    echo "$OUTPUT"
    echo ""

    if echo "$OUTPUT" | grep -q "$expect_file"; then
        log_pass "$full_label"
    else
        log_fail "$full_label" "expected '$expect_file' in output"
    fi
}

run_test_all_backends() {
    local img="$1" fstype="$2" part="$3" expect_file="$4" label="$5"
    for backend in "${BACKENDS[@]}"; do
        run_test "$img" "$fstype" "$part" "$expect_file" "$label" "$backend"
    done
}

# ============================================================
# Test 1: ext4 whole-disk (part=0)
# ============================================================
echo "================================================================"
echo "=== Test 1: ext4 whole-disk image (part=0) ==="
echo "================================================================"

IMG="$TMPDIR/ext4_single.img"
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
/usr/sbin/mkfs.ext4 -F -q "$IMG"
sudo mount -o loop "$IMG" "$TMPDIR/mnt"
echo "Hello from AnyFS!" | sudo tee "$TMPDIR/mnt/hello.txt" > /dev/null
sudo mkdir -p "$TMPDIR/mnt/subdir"
sudo umount "$TMPDIR/mnt"

run_test_all_backends "$IMG" ext4 0 "hello.txt" "ext4 whole-disk (part=0)"

# ============================================================
# Test 2: xfs whole-disk (part=0)
# ============================================================
echo "================================================================"
echo "=== Test 2: xfs whole-disk image (part=0) ==="
echo "================================================================"

IMG="$TMPDIR/xfs_single.img"
dd if=/dev/zero of="$IMG" bs=1M count=320 status=none
/usr/sbin/mkfs.xfs -f "$IMG" > /dev/null
sudo mount -o loop "$IMG" "$TMPDIR/mnt"
echo "xfs content" | sudo tee "$TMPDIR/mnt/xfs_test.txt" > /dev/null
sudo umount "$TMPDIR/mnt"

run_test_all_backends "$IMG" xfs 0 "xfs_test.txt" "xfs whole-disk (part=0)"

# ============================================================
# Test 3: btrfs whole-disk (part=0)
# ============================================================
echo "================================================================"
echo "=== Test 3: btrfs whole-disk image (part=0) ==="
echo "================================================================"

if command -v /usr/sbin/mkfs.btrfs &>/dev/null; then
    IMG="$TMPDIR/btrfs_single.img"
    dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
    /usr/sbin/mkfs.btrfs -f -q "$IMG"
    sudo mount -o loop "$IMG" "$TMPDIR/mnt"
    echo "btrfs content" | sudo tee "$TMPDIR/mnt/btrfs_test.txt" > /dev/null
    sudo umount "$TMPDIR/mnt"

    run_test_all_backends "$IMG" btrfs 0 "btrfs_test.txt" "btrfs whole-disk (part=0)"
else
    echo "  [SKIP] mkfs.btrfs not found"
fi

# ============================================================
# Test 4: vfat whole-disk (part=0)
# ============================================================
echo "================================================================"
echo "=== Test 4: vfat whole-disk image (part=0) ==="
echo "================================================================"

IMG="$TMPDIR/vfat_single.img"
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
/usr/sbin/mkfs.vfat -F 32 "$IMG" > /dev/null
sudo mount -o loop "$IMG" "$TMPDIR/mnt"
echo "vfat content" | sudo tee "$TMPDIR/mnt/VFAT_TEST.TXT" > /dev/null
sudo umount "$TMPDIR/mnt"

run_test_all_backends "$IMG" vfat 0 "VFAT_TEST.TXT" "vfat/FAT32 whole-disk (part=0)"

# ============================================================
# Test 5: GPT mixed partitions (ext4 + xfs + btrfs + vfat)
# ============================================================
echo "================================================================"
echo "=== Test 5: GPT mixed-partition image (ext4 + xfs + btrfs + vfat) ==="
echo "================================================================"

IMG="$TMPDIR/gpt_mixed.img"
dd if=/dev/zero of="$IMG" bs=1M count=1024 status=none

# Create GPT with 4 partitions (xfs needs 300M+)
cat <<EOF | /usr/sbin/sfdisk -q "$IMG"
label: gpt
size=64M, type=linux
size=320M, type=linux
size=320M, type=linux
size=64M, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
EOF

# Setup loop device
LOOPDEV=$(sudo losetup --find --show --partscan "$IMG")
sleep 0.5

# Partition 1: ext4
echo "  Formatting p1 as ext4..."
sudo /usr/sbin/mkfs.ext4 -F -q "${LOOPDEV}p1"
sudo mount "${LOOPDEV}p1" "$TMPDIR/mnt"
echo "GPT part1 ext4" | sudo tee "$TMPDIR/mnt/gpt_ext4.txt" > /dev/null
sudo umount "$TMPDIR/mnt"

# Partition 2: xfs
echo "  Formatting p2 as xfs..."
sudo /usr/sbin/mkfs.xfs -f "${LOOPDEV}p2" > /dev/null
sudo mount "${LOOPDEV}p2" "$TMPDIR/mnt"
echo "GPT part2 xfs" | sudo tee "$TMPDIR/mnt/gpt_xfs.txt" > /dev/null
sudo umount "$TMPDIR/mnt"

# Partition 3: btrfs
if command -v /usr/sbin/mkfs.btrfs &>/dev/null; then
    echo "  Formatting p3 as btrfs..."
    sudo /usr/sbin/mkfs.btrfs -f -q "${LOOPDEV}p3"
    sudo mount "${LOOPDEV}p3" "$TMPDIR/mnt"
    echo "GPT part3 btrfs" | sudo tee "$TMPDIR/mnt/gpt_btrfs.txt" > /dev/null
    sudo umount "$TMPDIR/mnt"
    HAS_BTRFS=1
fi

# Partition 4: vfat
echo "  Formatting p4 as vfat..."
sudo /usr/sbin/mkfs.vfat -F 32 "${LOOPDEV}p4" > /dev/null
sudo mount "${LOOPDEV}p4" "$TMPDIR/mnt"
echo "GPT part4 vfat" | sudo tee "$TMPDIR/mnt/GPT_VFAT.TXT" > /dev/null
sudo umount "$TMPDIR/mnt"

sudo losetup -d "$LOOPDEV"
LOOPDEV=""

# Run tests on each partition
run_test_all_backends "$IMG" ext4  1 "gpt_ext4.txt"  "GPT part=1 ext4"
run_test_all_backends "$IMG" xfs   2 "gpt_xfs.txt"   "GPT part=2 xfs"
if [[ -n "$HAS_BTRFS" ]]; then
    run_test_all_backends "$IMG" btrfs 3 "gpt_btrfs.txt" "GPT part=3 btrfs"
fi
run_test_all_backends "$IMG" vfat  4 "GPT_VFAT.TXT"  "GPT part=4 vfat"

# ============================================================
# Test 6: QEMU block backend (qcow2, vmdk, vdi)
# ============================================================
QEMU_BIN="${SCRIPT_DIR}/../builddir/test_qemu_mount"
QEMU_IMG="${SCRIPT_DIR}/../builddir/../scripts/../builddir/test_qemu_mount"

if [[ -x "${SCRIPT_DIR}/../builddir/test_qemu_mount" ]]; then
    echo ""
    echo "================================================================"
    echo "=== Test 6: QEMU block backend (qcow2, vmdk, vdi) ==="
    echo "================================================================"

    # Create a raw ext4 image as source
    QEMU_RAW="$TMPDIR/qemu_source.img"
    dd if=/dev/zero of="$QEMU_RAW" bs=1M count=32 status=none
    /usr/sbin/mkfs.ext4 -F -q "$QEMU_RAW"
    sudo mount -o loop "$QEMU_RAW" "$TMPDIR/mnt"
    echo "qemu_test_content" | sudo tee "$TMPDIR/mnt/qemu_test.txt" > /dev/null
    sudo umount "$TMPDIR/mnt"

    # Find qemu-img
    QEMU_IMG_BIN=""
    for p in ${QEMU_SRC}/build-anyfs2/qemu-img /usr/bin/qemu-img; do
        [[ -x "$p" ]] && QEMU_IMG_BIN="$p" && break
    done

    if [[ -n "$QEMU_IMG_BIN" ]]; then
        for fmt in qcow2 vmdk vdi; do
            QIMG="$TMPDIR/test.$fmt"
            "$QEMU_IMG_BIN" convert -f raw -O "$fmt" "$QEMU_RAW" "$QIMG" 2>/dev/null

            echo ""
            echo "--- Running: [qemu] $fmt ext4 ---"
            OUTPUT=$("${SCRIPT_DIR}/../builddir/test_qemu_mount" "$QIMG" ext4 2>&1)
            echo "$OUTPUT" | grep -v "^\[" | head -5
            echo ""

            if echo "$OUTPUT" | grep -q "qemu_test.txt"; then
                log_pass "[qemu] $fmt ext4"
            else
                log_fail "[qemu] $fmt ext4" "expected 'qemu_test.txt' in output"
            fi
        done
    else
        echo "  [SKIP] qemu-img not found, cannot convert formats"
    fi
else
    echo ""
    echo "  [SKIP] QEMU backend not compiled (test_qemu_mount not found)"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "================================================================"
echo " RESULTS: $PASS passed, $FAIL failed"
echo "================================================================"
[[ $FAIL -eq 0 ]]
