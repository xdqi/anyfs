#!/usr/bin/env python3
"""Write a disk image whose file names exercise every name encoding case.

Usage: make_names_image.py <out.img>

MBR disk, 13 MiB:
  p1  FAT12, written by hand (no tools needed):
        中文.txt   long name (UTF-16 on disk)          "fat-cn\\n"
        café.txt   long name                           "fat-cafe\\n"
        B2 E2 CA D4 . TXT   8.3 name only, GBK bytes   "fat-gbk\\n"
            (测试.TXT in codepage 936, ▓Γ╩╘.TXT in 437)
  p2  ext4, built by mkfs.ext4 -d from byte-named files:
        D6 D0 CE C4 .txt   GBK                         "ext4-gbk\\n"
        caf E9 .txt        Latin-1                     "ext4-latin1\\n"
        U+EF80 .txt        a real private-use char     "ext4-pua\\n"
        ED A0 80 .txt      an encoded surrogate        "ext4-surrogate\\n"
        plain.txt                                      "ext4-plain\\n"

Exit status 77 when mkfs.ext4 is missing (meson's "skip").
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
P1_START, P1_SECTORS = 2048, 8192
P2_START, P2_SECTORS = P1_START + P1_SECTORS, 16384
DISK_SECTORS = P2_START + P2_SECTORS

# FAT12 geometry of p1: 4-sector clusters, 1 reserved sector, 2 FATs of 6
# sectors, 16 root entries (1 sector); data starts at sector 14.
SPC, RESERVED, NFATS, FAT_SECTORS, ROOT_ENTRIES = 4, 1, 2, 6, 16
ROOT_SECTOR = RESERVED + NFATS * FAT_SECTORS
DATA_SECTOR = ROOT_SECTOR + ROOT_ENTRIES * 32 // SECTOR

FAT_FILES = [
    # (long name or None, 8.3 name bytes, content)
    ("中文.txt", b"CN~1    TXT", b"fat-cn\n"),
    ("café.txt", b"CAFE~1  TXT", b"fat-cafe\n"),
    (None, b"\xb2\xe2\xca\xd4    TXT", b"fat-gbk\n"),
]

EXT4_FILES = [
    (b"\xd6\xd0\xce\xc4.txt", b"ext4-gbk\n"),
    (b"caf\xe9.txt", b"ext4-latin1\n"),
    (".txt".encode(), b"ext4-pua\n"),
    (b"\xed\xa0\x80.txt", b"ext4-surrogate\n"),
    (b"plain.txt", b"ext4-plain\n"),
]


def lfn_checksum(short):
    s = 0
    for c in short:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_slots(name, short):
    """Directory entries (in on-disk order) holding `name` as a long name."""
    units = list(struct.unpack("<%dH" % (len(name.encode("utf-16-le")) // 2),
                               name.encode("utf-16-le")))
    n = (len(units) + 12) // 13
    units += [0x0000] if len(units) % 13 else []
    units += [0xFFFF] * (n * 13 - len(units))
    csum = lfn_checksum(short)
    slots = []
    for i in range(n):
        part = units[i * 13:(i + 1) * 13]
        seq = (i + 1) | (0x40 if i == n - 1 else 0)
        e = bytearray(32)
        e[0] = seq
        e[1:11] = struct.pack("<5H", *part[0:5])
        e[11] = 0x0F
        e[13] = csum
        e[14:26] = struct.pack("<6H", *part[5:11])
        e[28:32] = struct.pack("<2H", *part[11:13])
        slots.append(bytes(e))
    return list(reversed(slots))


def fat12_set(fat, n, v):
    off = n * 3 // 2
    if n & 1:
        fat[off] = (fat[off] & 0x0F) | ((v & 0x0F) << 4)
        fat[off + 1] = (v >> 4) & 0xFF
    else:
        fat[off] = v & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F)


def fat12_image():
    img = bytearray(P1_SECTORS * SECTOR)
    bs = img[0:SECTOR]
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"ANYFSTST"
    struct.pack_into("<HBHBHHBHHHI", bs, 11, SECTOR, SPC, RESERVED, NFATS,
                     ROOT_ENTRIES, P1_SECTORS, 0xF8, FAT_SECTORS, 32, 2,
                     P1_START)
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x5A17E5)
    bs[43:54] = b"NAMES      "
    bs[54:62] = b"FAT12   "
    bs[510:512] = b"\x55\xaa"
    img[0:SECTOR] = bs

    fat = bytearray(FAT_SECTORS * SECTOR)
    fat12_set(fat, 0, 0xFF8)
    fat12_set(fat, 1, 0xFFF)
    root = bytearray()
    for i, (long_name, short, content) in enumerate(FAT_FILES):
        cluster = 2 + i
        fat12_set(fat, cluster, 0xFFF)
        if long_name:
            for slot in lfn_slots(long_name, short):
                root += slot
        e = bytearray(32)
        e[0:11] = short
        e[11] = 0x20
        struct.pack_into("<HI", e, 26, cluster, len(content))
        root += e
        off = (DATA_SECTOR + (cluster - 2) * SPC) * SECTOR
        img[off:off + len(content)] = content
    assert len(root) <= ROOT_ENTRIES * 32
    for i in range(NFATS):
        off = (RESERVED + i * FAT_SECTORS) * SECTOR
        img[off:off + len(fat)] = fat
    img[ROOT_SECTOR * SECTOR:ROOT_SECTOR * SECTOR + len(root)] = root
    return img


def ext4_image(tmp):
    mkfs = shutil.which("mkfs.ext4") or next(
        (p for p in ("/usr/sbin/mkfs.ext4", "/sbin/mkfs.ext4")
         if os.path.exists(p)), None)
    if not mkfs:
        print("skip: mkfs.ext4 not found", file=sys.stderr)
        sys.exit(77)
    root = os.path.join(tmp, "root").encode()
    os.mkdir(root)
    for name, content in EXT4_FILES:
        with open(os.path.join(root, name), "wb") as f:
            f.write(content)
    out = os.path.join(tmp, "ext4.img")
    subprocess.run([mkfs, "-q", "-F", "-d", root, "-U", "clear",
                    "-E", "hash_seed=00000000-0000-0000-0000-000000000000",
                    out, "%dk" % (P2_SECTORS * SECTOR // 1024)],
                   check=True)
    with open(out, "rb") as f:
        return f.read()


def mbr():
    m = bytearray(SECTOR)
    struct.pack_into("<I", m, 440, 0x4E414D45)
    for i, (ptype, start, size) in enumerate(
            [(0x01, P1_START, P1_SECTORS), (0x83, P2_START, P2_SECTORS)]):
        e = 446 + 16 * i
        m[e + 4] = ptype
        struct.pack_into("<II", m, e + 8, start, size)
    m[510:512] = b"\x55\xaa"
    return m


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        ext4 = ext4_image(tmp)
    disk = bytearray(DISK_SECTORS * SECTOR)
    disk[0:SECTOR] = mbr()
    disk[P1_START * SECTOR:P2_START * SECTOR] = fat12_image()
    disk[P2_START * SECTOR:P2_START * SECTOR + len(ext4)] = ext4
    tmp_out = sys.argv[1] + ".tmp"
    with open(tmp_out, "wb") as f:
        f.write(disk)
    os.replace(tmp_out, sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
