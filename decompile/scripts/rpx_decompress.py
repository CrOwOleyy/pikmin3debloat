#!/usr/bin/env python3
"""Decompress a Wii U RPX/RPL into a plain ELF (same output as `wiiurpxtool -d`).

Sections flagged SHF_RPL_ZLIB hold a 4-byte big-endian decompressed size followed by a zlib
stream. The output keeps the ELF header and section header table in place, writes the section
data in file order (each aligned to 0x40), clears the zlib flag and rewrites the RPL CRC table.

Usage: python rpx_decompress.py <carrot.rpx> <carrot.elf>
"""
import struct
import sys
import zlib

SHF_RPL_ZLIB = 0x08000000
SHT_RPL_CRCS = 0x80000003


def decompress(data: bytes) -> bytes:
    if data[:4] != b"\x7fELF" or struct.unpack_from(">H", data, 0x10)[0] != 0xFE01:
        raise ValueError("not a Wii U RPX/RPL file")
    shoff, = struct.unpack_from(">I", data, 0x20)
    shentsize, shnum = struct.unpack_from(">HH", data, 0x2E)
    sections = [list(struct.unpack_from(">10I", data, shoff + i * shentsize)) for i in range(shnum)]

    out = bytearray(data[:0x34])
    out += bytes(12)
    out += bytes(max(0, shoff + shnum * shentsize - len(out)))

    crcs = [0] * shnum
    crc_offset = 0
    for index in sorted((i for i, s in enumerate(sections) if s[4] != 0), key=lambda i: sections[i][4]):
        sh = sections[index]
        offset, size = sh[4], sh[5]
        sh[4] = len(out)
        if sh[2] & SHF_RPL_ZLIB:
            sh[5], = struct.unpack_from(">I", data, offset)
            payload = zlib.decompress(data[offset + 4:offset + size])
            sh[2] &= ~SHF_RPL_ZLIB
        else:
            payload = data[offset:offset + size]
        out += payload
        crcs[index] = zlib.crc32(payload)
        out += bytes(-len(out) % 0x40)
        if (sh[1] & SHT_RPL_CRCS) == SHT_RPL_CRCS:
            crcs[index] = 0
            crc_offset = sh[4]

    for i, sh in enumerate(sections):
        struct.pack_into(">10I", out, shoff + i * shentsize, *sh)
    if crc_offset:
        struct.pack_into(">%dI" % shnum, out, crc_offset, *crcs)
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    elf = decompress(data)
    with open(sys.argv[2], "wb") as f:
        f.write(elf)
    print(f"{sys.argv[1]} -> {sys.argv[2]} ({len(data)} -> {len(elf)} bytes)")


if __name__ == "__main__":
    main()
