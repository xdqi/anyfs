#!/usr/bin/env bash
# Build the device-I/O test fixture: tests/macos/make-fixtures.sh's
# reproducible parts.img (GPT: BIOS boot, vfat ESP, ext4 fixroot with fixed
# ids; reference data committed in tests/macos/reference/expected.json),
# the same disk as parts.vhdx for Windows (Mount-DiskImage), and
# expected.json in check-smoke.mjs's format, taken from that reference.
#
# scripts/ci/test-device.{sh,ps1} attach it as a read-only block device;
# smoke-device.sh and the Playwright device spec then read it through the
# packaged app.
#
# Usage: make-device-fixture.sh <dir>   (needs sfdisk, mkfs.vfat, mkfs.ext4,
#                                        python3, qemu-img)
set -euo pipefail

dir="${1:?usage: make-device-fixture.sh <dir>}"
repo_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
command -v qemu-img > /dev/null || { echo "qemu-img not found; install qemu-utils" >&2; exit 1; }

mkdir -p "$dir"
bash "$repo_root/tests/macos/make-fixtures.sh" "$dir"
rm -f "$dir/parts-zlib.dmg" "$dir/parts-bz2.dmg"
qemu-img convert -f raw -O vhdx -o subformat=fixed "$dir/parts.img" "$dir/parts.vhdx"

python3 - "$repo_root/tests/macos/reference/expected.json" "$dir/expected.json" << 'PY'
import hashlib, json, os, sys
ref, out = sys.argv[1], sys.argv[2]
img = next(i for i in json.load(open(ref))["images"] if i["file"] == "parts.img")
part = next(p for p in img["parts"] if p["index"] == img["enter"])
f = max(img["files"], key=lambda f: f["size"])
raw = os.path.join(os.path.dirname(out), "parts.img")
json.dump({
    "image": "parts.img",
    "imageSha256": hashlib.sha256(open(raw, "rb").read()).hexdigest(),
    "imageSize": os.path.getsize(raw),
    "partitions": [{"fstype": p["fstype"], "label": p["label"]} for p in img["parts"] if p["fstype"]],
    "part": part["label"],
    "entries": [e for d in img["dirs"] if d["path"] == "" for e in d["contains"]],
    "read": f["path"],
    "size": f["size"],
    "sha256": f["sha256"],
}, open(out, "w"), indent=2)
PY
echo "make-device-fixture: $dir/{parts.img,parts.vhdx,expected.json}"
cat "$dir/expected.json"
