#!/usr/bin/env python3
"""List the player-visible English text of the game, from its unpacked asset files.

Usage: extract_text.py <assets/bin/Data dir> <decompiled dir> <out.json>
Dialogue, items and quests are XML text assets; menus are serialized strings in the scenes;
the rest are literals in the game's code.
"""
import collections
import glob
import json
import os
import re
import sys

STRING = re.compile(rb"([\x01-\xff][\x00-\xff]{3})")
WORDY = re.compile(r"^[A-Z0-9\"'(\[].*[a-z].*$", re.S)


def load_assets(base):
    parts = collections.defaultdict(list)
    for path in glob.glob(os.path.join(base, "*")):
        if not os.path.isfile(path) or path.endswith(".resS"):
            continue
        name = os.path.basename(path)
        m = re.match(r"(.+)\.split(\d+)$", name)
        if m:
            parts[m.group(1)].append((int(m.group(2)), path))
        else:
            parts[name].append((0, path))
    for name, files in parts.items():
        yield name, b"".join(open(p, "rb").read() for _, p in sorted(files))


def serialized_strings(data):
    """Unity strings: little-endian length, then the bytes, aligned to 4."""
    out = []
    i = 0
    n = len(data)
    while i + 8 < n:
        size = int.from_bytes(data[i:i + 4], "little")
        if 3 <= size < 2000 and i + 4 + size <= n:
            raw = data[i + 4:i + 4 + size]
            if all(b >= 0x20 or b in (9, 10, 13) for b in raw) and raw.count(b" ") + raw.count(b"\n") >= 1:
                try:
                    out.append(raw.decode("utf-8"))
                    i += 4 + ((size + 3) & ~3)
                    continue
                except UnicodeDecodeError:
                    pass
        i += 4
    return out


def xml_text(data):
    text = data.decode("utf-8", "ignore")
    return [m.group(1).strip() for m in re.finditer(r">([^<>]*[A-Za-z][^<>]*)<", text)], [
        m.group(2).strip() for m in re.finditer(r'\b(name|desc|title|text|displayName|label|description)="([^"]*[a-z][^"]*)"', text)]


def main():
    base, code, out = sys.argv[1:4]
    found = collections.defaultdict(set)
    for name, data in load_assets(base):
        if b"<?xml" in data[:4096] or b"<Dialog" in data or b"<Item" in data or b"<Text" in data[:200000]:
            a, b = xml_text(data)
            for s in a + b:
                found[s].add("xml")
        if name.startswith("level") or name.startswith("sharedassets") or name == "mainData":
            for s in serialized_strings(data):
                found[s.strip()].add("scene")
    literal = re.compile(r'"((?:[^"\\]|\\.){3,})"')
    for path in glob.glob(os.path.join(code, "*", "*.cs")):
        with open(path, encoding="utf-8", errors="ignore") as fh:
            for m in literal.finditer(fh.read()):
                s = m.group(1)
                if " " in s:
                    found[s.replace('\\"', '"').replace("\\n", "\n")].add("code")
    keep = {s: sorted(v) for s, v in found.items() if WORDY.match(s) and not s.startswith(("Assets/", "UnityEngine", "Shader", "[ravenmod"))}
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(keep, fh, ensure_ascii=False, indent=0, sort_keys=True)
    words = sum(len(s.split()) for s in keep)
    by = collections.Counter(k for v in keep.values() for k in v)
    print(f"{len(keep)} strings, {words} words, by source {dict(by)}")


if __name__ == "__main__":
    main()
