#!/usr/bin/env python3
"""Writes a CSO (DEFLATE, version 1) or ZSO (LZ4) copy of an ISO to test the player's readers.

    mkcso.py in.iso out.cso
    mkcso.py in.iso out.zso      (needs the lz4 command; uses 64 KiB blocks)

Not a replacement for maxcso: it is simple and slow, and only meant for tests."""
import struct
import subprocess
import sys
import zlib

SECTOR = 2048


def write(out, magic, total, block, blocks):
    """blocks yields (data, plain) per block in order."""
    count = (total + block - 1) // block
    header = magic + struct.pack("<IQIBB2x", 24, total, block, 1, 0)
    index_at = len(header)
    out.write(header + b"\0" * (4 * (count + 1)))
    index = []
    for data, plain in blocks:
        index.append(out.tell() | (0x80000000 if plain else 0))
        out.write(data)
    index.append(out.tell())
    assert len(index) == count + 1, (len(index), count)
    out.seek(index_at)
    out.write(struct.pack("<%dI" % len(index), *index))


def cso_blocks(f, block):
    while True:
        raw = f.read(block)
        if not raw:
            return
        raw = raw.ljust(block, b"\0")
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(raw) + c.flush()
        yield (raw, True) if len(z) >= block else (z, False)


def zso_blocks(path, block):
    """Runs lz4 with independent 64 KiB blocks and takes the blocks out of its frame."""
    p = subprocess.run(["lz4", "-q", "-9", "-B4", "-BI", "--no-frame-crc", "-c", path], check=True, stdout=subprocess.PIPE)
    data = p.stdout
    assert data[:4] == b"\x04\x22\x4d\x18", "not an LZ4 frame"
    flg = data[4]
    pos = 4 + 2 + (8 if flg & 0x08 else 0) + 1
    while True:
        (size,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if size == 0:
            return
        n = size & 0x7FFFFFFF
        chunk = data[pos:pos + n]
        pos += n
        plain = bool(size & 0x80000000)
        yield (chunk.ljust(block, b"\0") if plain else chunk), plain


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        f.seek(0, 2)
        total = f.tell()
        f.seek(0)
        with open(dst, "wb") as out:
            if dst.lower().endswith(".zso"):
                write(out, b"ZISO", total, 65536, zso_blocks(src, 65536))
            else:
                write(out, b"CISO", total, SECTOR, cso_blocks(f, SECTOR))


if __name__ == "__main__":
    main()
