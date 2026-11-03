#!/usr/bin/env python3
"""The game's Narkisim font, inside sharedassets0.assets.

  patch_font.py extract <original.apk> <out.ttf>        the font as the game ships it
  patch_font.py <original.apk> <new.ttf> <stage Data dir>  writes the accented one in its place

Both copies of the font (nrkis, nrkissmall) are the same bytes, so nothing else in the file moves.
"""
import hashlib
import os
import sys
import zipfile

NAME = "assets/bin/Data/sharedassets0.assets"
OLD_SHA1 = "4ea9a32bae"  # start of the digest of the original font
OLD_SIZE = 56832


def copies(data):
    pos = 0
    while True:
        pos = data.find(b"\x00\x01\x00\x00", pos)
        if pos < 0:
            return
        if hashlib.sha1(data[pos:pos + OLD_SIZE]).hexdigest().startswith(OLD_SHA1):
            yield pos
            pos += OLD_SIZE
        else:
            pos += 4


def read(apk):
    with zipfile.ZipFile(apk) as z:
        return z.read(NAME)


def extract(apk, dst):
    data = read(apk)
    found = list(copies(data))
    if not found:
        raise SystemExit("the game's font was not found; is this the 1.26 APK?")
    with open(dst, "wb") as fh:
        fh.write(data[found[0]:found[0] + OLD_SIZE])
    print(f"game font extracted ({OLD_SIZE} bytes)")


def patch(apk, ttf, data_dir):
    data = read(apk)
    new = open(ttf, "rb").read()
    if len(new) != OLD_SIZE:
        raise SystemExit(f"{ttf} is {len(new)} bytes, expected {OLD_SIZE}")
    found = list(copies(data))
    if len(found) != 2:
        raise SystemExit(f"expected 2 copies of the font, found {len(found)}")
    for pos in found:
        data = data[:pos] + new + data[pos + OLD_SIZE:]
    with open(os.path.join(data_dir, "sharedassets0.assets"), "wb") as fh:
        fh.write(data)
    print(f"accented font written over {len(found)} copies")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "extract":
        extract(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4:
        patch(*sys.argv[1:4])
    else:
        raise SystemExit(__doc__)
