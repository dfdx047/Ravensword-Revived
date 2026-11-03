#!/usr/bin/env python3
"""Patch Unity 4.1.2 player data in place, without changing any object's size.

Usage: patch_unity_data.py <orig mainData> <orig settings.xml> <out dir> [--default-aa N]

mainData: every QualitySettings level is set to the values in LEVEL.
settings.xml: 32-bit colour, 24-bit depth and MSAA for the Android EGL config.

The file is edited byte by byte at the offset UnityPy reports, and re-read afterwards
to confirm the result, so nothing else in mainData can change.
"""
import argparse
import os
import re
import struct

import UnityPy

LEVEL = {
    "pixelLightCount": 8,
    "shadows": 2,            # hard and soft
    "shadowResolution": 3,   # very high
    "shadowProjection": 1,   # stable fit
    "shadowCascades": 4,
    "shadowDistance": 1000.0,
    "blendWeights": 4,
    "textureQuality": 0,     # full resolution
    "anisotropicTextures": 2,  # forced on
    "antiAliasing": 8,
    "softParticles": True,
    "softVegetation": True,
    "vSyncCount": 0,         # let targetFrameRate decide
    "lodBias": 10.0,
    "maximumLODLevel": 0,
    "particleRaycastBudget": 4096,
}
TOP_QUALITY = 5


def patch_quality(data: bytearray, start: int, size: int) -> int:
    p = start
    struct.pack_into("<i", data, p, TOP_QUALITY)
    p += 4
    count = struct.unpack_from("<i", data, p)[0]
    p += 4
    if count != 6:
        raise SystemExit(f"expected 6 quality levels, found {count}")
    for _ in range(count):
        n = struct.unpack_from("<i", data, p)[0]
        p += 4 + n
        p = start + ((p - start + 3) & ~3)
        struct.pack_into(
            "<iiiiifiiii", data, p,
            LEVEL["pixelLightCount"], LEVEL["shadows"], LEVEL["shadowResolution"],
            LEVEL["shadowProjection"], LEVEL["shadowCascades"], LEVEL["shadowDistance"],
            LEVEL["blendWeights"], LEVEL["textureQuality"], LEVEL["anisotropicTextures"],
            LEVEL["antiAliasing"],
        )
        p += 40
        data[p] = int(LEVEL["softParticles"])
        data[p + 1] = int(LEVEL["softVegetation"])
        p += 4  # two bools plus alignment
        struct.pack_into(
            "<ifii", data, p,
            LEVEL["vSyncCount"], LEVEL["lodBias"], LEVEL["maximumLODLevel"],
            LEVEL["particleRaycastBudget"],
        )
        p += 16
    if p + 4 != start + size:  # m_StrippedMaximumLODLevel is the last field
        raise SystemExit(f"QualitySettings layout mismatch: ended at {p - start}, size {size}")
    return count


def find_quality(path: str):
    env = UnityPy.load(path)
    for f in env.files.values():
        for o in f.objects.values():
            if o.type.name == "QualitySettings":
                return o
    raise SystemExit("QualitySettings not found in " + path)


def verify(path: str) -> None:
    d = find_quality(path).parse_as_dict()
    if d["m_CurrentQuality"] != TOP_QUALITY:
        raise SystemExit("verification failed: m_CurrentQuality")
    for level in d["m_QualitySettings"]:
        for key, want in LEVEL.items():
            got = level[key]
            ok = abs(got - want) < 1e-3 if isinstance(want, float) else got == want
            if not ok:
                raise SystemExit(f"verification failed: {level['name']}.{key}={got}, wanted {want}")
    print("verified QualitySettings:", ", ".join(l["name"] for l in d["m_QualitySettings"]))


def patch_settings(src: str, dst: str, default_aa: int) -> None:
    with open(src, encoding="utf-8") as fh:  # keeps the BOM as a character
        text = fh.read()
    subs = [
        (r'(<bool name="32bit_display">)\w+', r"\g<1>True"),
        (r'(<bool name="24bit_depth">)\w+', r"\g<1>True"),
        (r'(<integer name="default_aa">)\d+', rf"\g<1>{default_aa}"),
    ]
    for pattern, repl in subs:
        text, n = re.subn(pattern, repl, text)
        if n != 1:
            raise SystemExit("settings.xml: no match for " + pattern)
    with open(dst, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print(f"patched settings.xml: 32bit_display, 24bit_depth, default_aa={default_aa}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("main_data")
    ap.add_argument("settings_xml")
    ap.add_argument("out_dir")
    ap.add_argument("--default-aa", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)

    o = find_quality(a.main_data)
    with open(a.main_data, "rb") as fh:
        data = bytearray(fh.read())
    original_len = len(data)
    count = patch_quality(data, o.byte_start, o.byte_size)
    if len(data) != original_len:
        raise SystemExit("mainData size changed")
    out = os.path.join(a.out_dir, "mainData")
    with open(out, "wb") as fh:
        fh.write(data)
    print(f"patched mainData: {count} quality levels at offset {o.byte_start}")
    verify(out)
    patch_settings(a.settings_xml, os.path.join(a.out_dir, "settings.xml"), a.default_aa)


if __name__ == "__main__":
    main()
