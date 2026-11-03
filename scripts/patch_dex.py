#!/usr/bin/env python3
"""Force a 32-bit color surface in classes.dex.

Usage: patch_dex.py <in classes.dex> <out classes.dex>

com.unity3d.player.aa.a(Z)Z decides whether to recreate the GL surface with the color depth
Unity asks for. The build asks for 16 bits, which produced visible banding. Removing its
`if-eqz v6` makes the method behave as if 32 bits had been requested, every time.
"""
import hashlib
import struct
import sys
import zlib

# Offset of the instruction inside the method, from dexdump of the 1.26 APK.
OFFSET = 0x0B8278
EXPECTED = bytes.fromhex("38061300")  # if-eqz v6, +0x13
NOPS = bytes.fromhex("00000000")


def main() -> None:
    src, dst = sys.argv[1:3]
    with open(src, "rb") as fh:
        data = bytearray(fh.read())
    found = bytes(data[OFFSET:OFFSET + 4])
    if found != EXPECTED:
        raise SystemExit(f"unexpected bytes at {OFFSET:#x}: {found.hex()} (different game version?)")
    data[OFFSET:OFFSET + 4] = NOPS
    data[12:32] = hashlib.sha1(bytes(data[32:])).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(bytes(data[12:])) & 0xFFFFFFFF)
    with open(dst, "wb") as fh:
        fh.write(data)
    print("patched classes.dex: GL surface always 32-bit")


if __name__ == "__main__":
    main()
