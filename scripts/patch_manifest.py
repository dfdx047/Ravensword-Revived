#!/usr/bin/env python3
"""Patch integer attributes of a binary AndroidManifest.xml in place.

Usage: patch_manifest.py <in> <out> [--debuggable]

targetSdkVersion becomes 28 (Android 15 blocks old targets and warns below 28), minSdkVersion
becomes 24 because the bridge is built for android-24, and maxSdkVersion is lifted.
"""
import struct
import sys

RES_MIN_SDK = 0x0101020C
RES_TARGET_SDK = 0x01010270
RES_MAX_SDK = 0x01010271
RES_DEBUGGABLE = 0x0101000F


def main() -> None:
    src, dst = sys.argv[1:3]
    debuggable = "--debuggable" in sys.argv[3:]
    with open(src, "rb") as fh:
        data = bytearray(fh.read())

    wanted = {RES_MIN_SDK: 24, RES_TARGET_SDK: 28, RES_MAX_SDK: 35}
    if debuggable:
        wanted[RES_DEBUGGABLE] = 0xFFFFFFFF

    pos = 8
    resmap = []
    done = set()
    while pos < len(data):
        ctype, header, size = struct.unpack_from("<HHI", data, pos)
        if ctype == 0x0180:  # resource map
            count = (size - header) // 4
            resmap = list(struct.unpack_from(f"<{count}I", data, pos + header))
        elif ctype == 0x0102:  # start element
            attr_start, attr_size, attr_count = struct.unpack_from("<HHH", data, pos + header + 8)
            first = pos + header + attr_start
            for i in range(attr_count):
                at = first + i * attr_size
                name = struct.unpack_from("<I", data, at + 4)[0]
                res = resmap[name] if name < len(resmap) else 0
                if res in wanted:
                    struct.pack_into("<I", data, at + 16, wanted[res])
                    done.add(res)
        pos += size

    missing = {hex(k) for k in wanted if k not in done}
    if RES_DEBUGGABLE in wanted and RES_DEBUGGABLE not in done:
        raise SystemExit("android:debuggable attribute not present in the manifest")
    if RES_TARGET_SDK not in done:
        raise SystemExit("targetSdkVersion not found; missing: " + ", ".join(sorted(missing)))
    with open(dst, "wb") as fh:
        fh.write(data)
    print(f"patched manifest: minSdk 24, targetSdk 28, maxSdk 35, debuggable={debuggable}")


if __name__ == "__main__":
    main()
