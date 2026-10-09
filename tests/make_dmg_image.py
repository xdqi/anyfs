#!/usr/bin/env python3
"""Wrap a raw disk image in an Apple UDIF disk image (.dmg).

Usage: make_dmg_image.py [--codec zlib|bz2] RAW OUT.dmg

Writes the layout hdiutil produces for a compressed read-only image: the data
fork (one compressed chunk per 1 MiB of input, all-zero chunks stored as
zero-fill entries), an XML property list whose resource-fork/blkx entry holds
the 'mish' chunk table, and the 512-byte 'koly' trailer. zlib gives a UDZO
image (QEMU's dmg driver), bz2 a UDBZ one (its dmg-bz2 driver). CRC32
checksums are filled in, so `hdiutil verify` on a Mac accepts the image too.
All integers are big-endian. The tests use it to cover the DMG format with
the same partitions as the raw fixture it wraps.
"""
import argparse
import binascii
import bz2
import plistlib
import struct
import uuid
import zlib

SECTOR = 512
CHUNK_SECTORS = 2048  # 1 MiB per chunk, as hdiutil

UDZE = 0x00000000  # zero fill
UDZO = 0x80000005  # zlib
UDBZ = 0x80000006  # bzip2
UDLE = 0xFFFFFFFF  # last entry

CRC32 = 2  # UDIF checksum type


def checksum(crc):
    """UDIF checksum record: type, size in bits, 32 u32 words."""
    return struct.pack(">II", CRC32, 32) + struct.pack(">I", crc) + bytes(124)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--codec", choices=("zlib", "bz2"), default="zlib")
    ap.add_argument("raw")
    ap.add_argument("out")
    a = ap.parse_args()

    with open(a.raw, "rb") as f:
        raw = f.read()
    if len(raw) % SECTOR:
        raw += bytes(SECTOR - len(raw) % SECTOR)
    sectors = len(raw) // SECTOR
    ctype, compress = {
        "zlib": (UDZO, lambda b: zlib.compress(b, 9)),
        "bz2": (UDBZ, lambda b: bz2.compress(b, 9)),
    }[a.codec]

    fork = bytearray()
    chunks = []
    for first in range(0, sectors, CHUNK_SECTORS):
        n = min(CHUNK_SECTORS, sectors - first)
        data = raw[first * SECTOR:(first + n) * SECTOR]
        if data.count(0) == len(data):
            chunks.append((UDZE, first, n, len(fork), 0))
            continue
        blob = compress(data)
        chunks.append((ctype, first, n, len(fork), len(blob)))
        fork += blob
    chunks.append((UDLE, sectors, 0, len(fork), 0))

    data_crc = binascii.crc32(raw)
    mish = struct.pack(">4sIQQQII", b"mish", 1, 0, sectors, 0, 0, 0)
    mish += bytes(24) + checksum(data_crc) + struct.pack(">I", len(chunks))
    for t, first, n, off, length in chunks:
        mish += struct.pack(">IIQQQQ", t, 0, first, n, off, length)

    plist = plistlib.dumps({
        "resource-fork": {
            "blkx": [{
                "Attributes": "0x0050",
                "CFName": "whole disk (anyfs test image)",
                "Data": mish,
                "ID": "-1",
                "Name": "whole disk (anyfs test image)",
            }],
        },
    })

    xml_offset = len(fork)
    # The master checksum covers the blkx checksums, in order.
    master = binascii.crc32(struct.pack(">I", data_crc))
    koly = struct.pack(">4sIIIQQQQQII", b"koly", 4, 512, 1, 0, 0, len(fork), 0, 0, 1, 1)
    koly += uuid.uuid4().bytes + checksum(binascii.crc32(fork))
    koly += struct.pack(">QQ", xml_offset, len(plist)) + bytes(120)
    koly += checksum(master) + struct.pack(">IQ", 1, sectors) + bytes(12)
    assert len(koly) == 512, len(koly)

    with open(a.out, "wb") as f:
        f.write(fork)
        f.write(plist)
        f.write(koly)


if __name__ == "__main__":
    main()
