#!/usr/bin/env bash
# Attach a disk image as a READ-ONLY block device for device-I/O tests, and
# detach it again (Linux, macOS; Windows: test-device.ps1). CI only: needs
# passwordless sudo.
#
#   attach [--nbd] <image> <state-dir>
#       Linux  sudo losetup --read-only --partscan --find --show
#              --nbd: sudo qemu-nbd --read-only on a free /dev/nbdN instead.
#              drivelist hides loop devices (on Ubuntu they are mostly
#              snaps), so the GUI test needs a device type it lists.
#       macOS  hdiutil attach -readonly -nomount
#     The device is taken only from that command's own answer for this image
#     and then verified: it is backed by <image>, has its size, and its first
#     MiB equals the image's. No device number is guessed, and nothing else
#     (the runner's system disk in particular) is touched.
#   perm deny|allow <state-dir>
#     Owner/mode of the test device nodes only: deny = root-owned 0600,
#     allow = owned by the calling user (mode 0600). Prints ls -l.
#   detach <state-dir>
#     Detaches, checks the device is gone and that the image's SHA-256 is
#     what it was before attach (the test did not write to it). device.env
#     marks an attached device; device.json stays as a record.
#
# <state-dir>/device.env (sourceable) and device.json describe the device:
#   ANYFS_TEST_DEVICE        whole disk (/dev/loopN, /dev/diskN)
#   ANYFS_TEST_RAW_DEVICE    macOS raw node (/dev/rdiskN), else empty
#   ANYFS_TEST_PART_DEVICES  partition nodes in table order, space-separated
#   ANYFS_TEST_FIXTURE       backing image, ANYFS_TEST_FIXTURE_SHA256 its hash
set -euo pipefail

die() { echo "test-device: $*" >&2; exit 1; }
usage() { die "usage: test-device.sh attach [--nbd] <image> <state-dir> | perm deny|allow <state-dir> | detach <state-dir>"; }

case "$(uname -s)" in
Linux) os=linux ;;
Darwin) os=darwin ;;
*) die "unsupported OS $(uname -s)" ;;
esac

sha() {
    if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
size_of() { if [[ $os == linux ]]; then stat -c %s "$1"; else stat -f %z "$1"; fi; }
# First MiB of a device (root) and of the image must match.
same_head() { cmp -s <(sudo dd if="$1" bs=1048576 count=1 2> /dev/null) <(head -c 1048576 "$2"); }

attach() {
    local kind=loop img state dev raw="" parts=() sum size
    if [[ "$1" == --nbd ]]; then kind=nbd; shift; fi
    [[ $kind == loop || $os == linux ]] || die "--nbd is Linux only"
    img="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    state="$2"
    [[ -f "$img" ]] || die "no image $img"
    mkdir -p "$state"
    [[ ! -e "$state/device.env" ]] || die "$state already describes an attached device"
    sum="$(sha "$img")"
    size="$(size_of "$img")"

    if [[ $kind == nbd ]]; then
        lsmod | grep -qw nbd || sudo modprobe nbd max_part=16
        local n
        dev=""
        for n in /sys/block/nbd*; do
            if [[ "$(cat "$n/size")" == 0 && ! -e "$n/pid" ]]; then dev="/dev/${n##*/}"; break; fi
        done
        [[ -n "$dev" ]] || die "no free /dev/nbdN"
        sudo qemu-nbd --read-only --format=raw --fork -c "$dev" "$img"
        printf 'ANYFS_TEST_KIND=nbd\nANYFS_TEST_DEVICE=%q\nANYFS_TEST_FIXTURE=%q\nANYFS_TEST_FIXTURE_SHA256=%q\n' \
            "$dev" "$img" "$sum" > "$state/device.env"
        local pidf="/sys/block/${dev#/dev/}/pid" i
        for i in 1 2 3 4 5 6 7 8 9 10; do [[ -e "$pidf" ]] && break; sleep 0.5; done
        [[ -e "$pidf" ]] || die "$dev did not connect"
        tr '\0' ' ' < "/proc/$(cat "$pidf")/cmdline" | grep -qF -- "$img" || die "$dev is not served from $img"
        [[ "$(cat "/sys/block/${dev#/dev/}/ro")" == 1 ]] || die "$dev is not read-only"
        [[ "$(sudo blockdev --getsize64 "$dev")" == "$size" ]] || die "$dev size differs from $img"
        sudo udevadm settle
        i=1
        while [[ -b "${dev}p$i" ]]; do parts+=("${dev}p$i"); i=$((i + 1)); done
    elif [[ $os == linux ]]; then
        dev="$(sudo losetup --read-only --partscan --find --show "$img")"
        [[ "$dev" == /dev/loop* ]] || die "losetup answered '$dev'"
        # Record it at once, so detach works even if a check below fails.
        printf 'ANYFS_TEST_DEVICE=%q\nANYFS_TEST_FIXTURE=%q\nANYFS_TEST_FIXTURE_SHA256=%q\n' \
            "$dev" "$img" "$sum" > "$state/device.env"
        [[ "$(losetup -n -O BACK-FILE "$dev")" == "$img" ]] || die "$dev is not backed by $img"
        [[ "$(losetup -n -O RO "$dev" | tr -d ' ')" == 1 ]] || die "$dev is not read-only"
        [[ "$(sudo blockdev --getsize64 "$dev")" == "$size" ]] || die "$dev size differs from $img"
        sudo udevadm settle
        local i=1
        while [[ -b "${dev}p$i" ]]; do parts+=("${dev}p$i"); i=$((i + 1)); done
    else
        local plist
        plist="$(hdiutil attach -readonly -nomount -noverify -noautofsck -plist "$img")"
        # Whole disk = the dev-entry without a slice suffix; partitions in order.
        dev="$(python3 -c '
import plistlib, sys, re
ents = plistlib.loads(sys.stdin.buffer.read())["system-entities"]
devs = [e["dev-entry"] for e in ents if "dev-entry" in e]
whole = [d for d in devs if re.fullmatch(r"/dev/disk[0-9]+", d)]
assert len(whole) == 1, devs
print(whole[0])
for d in sorted((d for d in devs if d != whole[0]), key=lambda d: int(d.rsplit("s", 1)[1])):
    print(d)' <<< "$plist")"
        mapfile_parts=()
        while IFS= read -r l; do mapfile_parts+=("$l"); done <<< "$dev"
        dev="${mapfile_parts[0]}"
        parts=("${mapfile_parts[@]:1}")
        raw="/dev/r${dev#/dev/}"
        printf 'ANYFS_TEST_DEVICE=%q\nANYFS_TEST_FIXTURE=%q\nANYFS_TEST_FIXTURE_SHA256=%q\n' \
            "$dev" "$img" "$sum" > "$state/device.env"
        # hdiutil's own table must map this image to this device.
        hdiutil info -plist | python3 -c '
import plistlib, sys
img, dev = sys.argv[1], sys.argv[2]
for i in plistlib.loads(sys.stdin.buffer.read())["images"]:
    if i.get("image-path") == img and any(e.get("dev-entry") == dev for e in i["system-entities"]):
        sys.exit(0)
sys.exit(f"{dev} is not attached from {img}")' "$img" "$dev"
        [[ "$(diskutil info -plist "$dev" | python3 -c 'import plistlib,sys; d = plistlib.loads(sys.stdin.buffer.read()); print(d.get("TotalSize") or d["Size"])')" == "$size" ]] \
            || die "$dev size differs from $img"
    fi
    same_head "$dev" "$img" || die "first MiB of $dev differs from $img"

    {
        printf 'ANYFS_TEST_KIND=%q\n' "$kind"
        printf 'ANYFS_TEST_DEVICE=%q\n' "$dev"
        printf 'ANYFS_TEST_RAW_DEVICE=%q\n' "$raw"
        printf 'ANYFS_TEST_PART_DEVICES=%q\n' "${parts[*]:-}"
        printf 'ANYFS_TEST_FIXTURE=%q\n' "$img"
        printf 'ANYFS_TEST_FIXTURE_SHA256=%q\n' "$sum"
    } > "$state/device.env"
    python3 - "$state/device.json" "$dev" "$raw" "$img" "$sum" "${parts[@]}" << 'EOF'
import json, sys
out, dev, raw, img, sum_, *parts = sys.argv[1:]
json.dump({"device": dev, "rawDevice": raw or None, "partitions": parts,
           "fixture": img, "fixtureSha256": sum_, "readOnly": True}, open(out, "w"), indent=2)
EOF
    echo "test-device: $img attached read-only as $dev${raw:+ ($raw)}; partitions: ${parts[*]:-none}"
    ls -l "$dev" ${raw:+"$raw"} ${parts[@]+"${parts[@]}"}
}

nodes() {
    # shellcheck disable=SC1091
    source "$1/device.env"
    # shellcheck disable=SC2086
    echo "$ANYFS_TEST_DEVICE" ${ANYFS_TEST_RAW_DEVICE:-} ${ANYFS_TEST_PART_DEVICES:-}
}

perm() {
    local mode="$1" state="$2" n
    for n in $(nodes "$state"); do
        case "$mode" in
        deny) sudo chown root "$n" ;;
        allow) sudo chown "$(id -un)" "$n" ;;
        *) usage ;;
        esac
        sudo chmod 600 "$n"
        ls -l "$n"
    done
}

detach() {
    local state="$1"
    [[ -f "$state/device.env" ]] || { echo "test-device: nothing attached in $state"; return 0; }
    # shellcheck disable=SC1091
    source "$state/device.env"
    if [[ "${ANYFS_TEST_KIND:-loop}" == nbd ]]; then
        sudo qemu-nbd -d "$ANYFS_TEST_DEVICE" || true
        local i
        for i in 1 2 3 4 5 6 7 8 9 10; do [[ ! -e "/sys/block/${ANYFS_TEST_DEVICE#/dev/}/pid" ]] && break; sleep 0.5; done
        [[ ! -e "/sys/block/${ANYFS_TEST_DEVICE#/dev/}/pid" ]] || die "$ANYFS_TEST_DEVICE is still connected"
    elif [[ $os == linux ]]; then
        sudo losetup -d "$ANYFS_TEST_DEVICE" || true
        [[ -z "$(losetup -j "$ANYFS_TEST_FIXTURE")" ]] || die "$ANYFS_TEST_FIXTURE is still attached"
    else
        # After `perm deny` the nodes are root's; detach as root then.
        hdiutil detach "$ANYFS_TEST_DEVICE" || sudo hdiutil detach -force "$ANYFS_TEST_DEVICE"
        if hdiutil info | grep -qF "$ANYFS_TEST_FIXTURE"; then die "$ANYFS_TEST_FIXTURE is still attached"; fi
    fi
    local now
    now="$(sha "$ANYFS_TEST_FIXTURE")"
    [[ "$now" == "$ANYFS_TEST_FIXTURE_SHA256" ]] \
        || die "$ANYFS_TEST_FIXTURE changed while attached ($ANYFS_TEST_FIXTURE_SHA256 -> $now)"
    rm -f "$state/device.env"
    echo "test-device: detached $ANYFS_TEST_DEVICE; $ANYFS_TEST_FIXTURE unchanged (sha256 $now)"
}

case "${1:-}" in
attach) [[ $# -eq 3 || $# -eq 4 ]] || usage; shift; attach "$@" ;;
perm) [[ $# -eq 3 ]] || usage; perm "$2" "$3" ;;
detach) [[ $# -eq 2 ]] || usage; detach "$2" ;;
*) usage ;;
esac
