#!/usr/bin/env python3
"""Decompress a CISO (.cso) PSP image back to a plain .iso."""
import struct
import sys
import zlib


def cso_to_iso(src, dst):
    with open(src, "rb") as f, open(dst, "wb") as out:
        magic, _hdr_size, total, block_size, ver, align = struct.unpack("<4sIQIBB2x", f.read(24))
        if magic != b"CISO":
            sys.exit(f"{src}: not a CISO file")
        nblocks = total // block_size
        index = struct.unpack(f"<{nblocks + 1}I", f.read(4 * (nblocks + 1)))
        for i in range(nblocks):
            plain = index[i] & 0x80000000
            start = (index[i] & 0x7FFFFFFF) << align
            end = (index[i + 1] & 0x7FFFFFFF) << align
            f.seek(start)
            data = f.read(end - start)
            if not plain:
                data = zlib.decompress(data, -15)  # raw deflate
            out.write(data[:block_size])
        print(f"{dst}: {total} bytes, {nblocks} blocks (CISO v{ver})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: cso2iso.py in.cso out.iso")
    cso_to_iso(sys.argv[1], sys.argv[2])
