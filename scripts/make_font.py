#!/usr/bin/env python3
"""Adds Portuguese accented letters to the game's Narkisim font.

Usage: make_font.py <in.ttf> <out.ttf>   (needs fontTools)
The font has no accented letters. They are built from its own base letters and accent glyphs.
Hinting and signature tables are dropped to pay for them, and the file is padded back to the
original size so that it can replace the old bytes inside sharedassets0.assets.
"""
import sys

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

LETTERS = {
    "á": ("a", "acute"), "à": ("a", "grave"), "â": ("a", "asciicircum"), "ã": ("a", "asciitilde"),
    "é": ("e", "acute"), "ê": ("e", "asciicircum"), "í": ("i", "acute"), "ó": ("o", "acute"),
    "ô": ("o", "asciicircum"), "õ": ("o", "asciitilde"), "ú": ("u", "acute"), "ü": ("u", "dieresis"),
    "ç": ("c", "cedilla"),
    "Á": ("A", "acute"), "À": ("A", "grave"), "Â": ("A", "asciicircum"), "Ã": ("A", "asciitilde"),
    "É": ("E", "acute"), "Ê": ("E", "asciicircum"), "Í": ("I", "acute"), "Ó": ("O", "acute"),
    "Ô": ("O", "asciicircum"), "Õ": ("O", "asciitilde"), "Ú": ("U", "acute"), "Ü": ("U", "dieresis"),
    "Ç": ("C", "cedilla"),
}


def contours(pen_value):
    out, cur = [], []
    for op, args in pen_value:
        cur.append((op, args))
        if op in ("closePath", "endPath"):
            out.append(cur)
            cur = []
    return out


def bounds(glyph, glyf):
    glyph.recalcBounds(glyf)
    return glyph.xMin, glyph.yMin, glyph.xMax, glyph.yMax


def main():
    src, dst = sys.argv[1:3]
    size = len(open(src, "rb").read())
    f = TTFont(src)
    for tag in ("DSIG", "fpgm", "prep", "cvt ", "GPOS", "GSUB", "GDEF"):
        if tag in f:
            del f[tag]
    f["post"].formatType = 3.0
    glyf, hmtx = f["glyf"], f["hmtx"]
    gs = f.getGlyphSet()
    order = list(f.getGlyphOrder())
    cmap = f.getBestCmap()
    named = dict(cmap)

    def gid(ch):
        return named[ord(ch)]

    accent_glyph = {"acute": named[0xB4], "grave": named[0x60], "asciicircum": named[0x5E],
                    "asciitilde": named[0x7E], "dieresis": named[0xA8], "cedilla": named[0xB8]}
    x_top = bounds(glyf[gid("x")], glyf)[3]
    cap_top = bounds(glyf[gid("H")], glyf)[3]
    for ch, (base, accent) in LETTERS.items():
        rec = DecomposingRecordingPen(gs)
        base_id = gid(base)
        gs[base_id].draw(rec)
        parts = contours(rec.value)
        if base == "i":
            # Drop the dot: the contour that sits entirely above the stem.
            def low(c):
                return min(p[1] for op, a in c for p in a if p is not None)
            parts = [c for c in parts if low(c) < x_top * 0.8]
        pen = TTGlyphPen(None)
        xs, ys = [], []
        for c in parts:
            for op, a in c:
                getattr(pen, op)(*a)
                for p in a:
                    if p is not None:
                        xs.append(p[0])
                        ys.append(p[1])
        bx0, bx1, by1 = min(xs), max(xs), max(ys)
        base_w = hmtx[base_id][0]
        rec2 = DecomposingRecordingPen(gs)
        gs[accent_glyph[accent]].draw(rec2)
        ax = [p[0] for op, a in rec2.value for p in a if p is not None]
        ay = [p[1] for op, a in rec2.value for p in a if p is not None]
        # The ASCII ^ and ~ are full-size symbols: on a letter they have to be much smaller.
        sx, sy = (0.62, 0.62) if accent == "asciicircum" else (0.62, 0.8) if accent == "asciitilde" else (1, 1)
        cx0, cy0 = (min(ax) + max(ax)) / 2, min(ay)
        rec2.value = [(op, tuple(None if p is None else (cx0 + (p[0] - cx0) * sx, cy0 + (p[1] - cy0) * sy) for p in a)) for op, a in rec2.value]
        ax = [p[0] for op, a in rec2.value for p in a if p is not None]
        ay = [p[1] for op, a in rec2.value for p in a if p is not None]
        dx = (bx0 + bx1) / 2 - (min(ax) + max(ax)) / 2
        if accent == "cedilla":
            dy = 0
        else:
            top = max(by1, x_top if base.islower() else cap_top)
            dy = top + (60 if base.islower() else 40) - min(ay)
        for op, a in rec2.value:
            getattr(pen, op)(*[None if p is None else (round(p[0] + dx), round(p[1] + dy)) for p in a])
        name = "uni%04X" % ord(ch)
        glyf[name] = pen.glyph()
        hmtx[name] = (base_w, round(bx0))
        order.append(name)
        for table in f["cmap"].tables:
            if table.isUnicode():
                table.cmap[ord(ch)] = name
    f.setGlyphOrder(order)
    glyf.glyphOrder = order
    f["maxp"].numGlyphs = len(order)
    f.save(dst)
    data = open(dst, "rb").read()
    if len(data) > size:
        raise SystemExit(f"font grew to {len(data)} > {size}")
    open(dst, "wb").write(data + bytes(size - len(data)))
    print(f"font {len(data)} bytes, padded to {size}")


if __name__ == "__main__":
    main()
