#!/usr/bin/env python3
"""Keeps the translation without shipping the game's own English text.

  lang_hash.py pack   <pt-BR.json> <original.apk> <pt-BR.hashed.json>
  lang_hash.py unpack <original.apk> <pt-BR.hashed.json> <out pt-BR.json>

The public file maps a hash of each English line to its Portuguese. `unpack` reads the
English lines from the user's own APK (text assets, scenes and the code's string literals),
hashes them and rebuilds the English-to-Portuguese dictionary that the mod compiles in.
Lines are hashed after the same whitespace folding the mod applies (ModLang.Norm).
Short pieces that the game assembles at run time ("Agility:", "East") are not in the APK as
whole strings; those are kept as plain text, since a word or two is no part of the game's script.
"""
import collections
import hashlib
import json
import re
import sys
import zipfile

DATA = "assets/bin/Data/"
HASH_LEN = 16  # hex digits of SHA-256


def norm(text):
    """ModLang.Norm: runs of space, tab and line breaks become one space, ends trimmed."""
    return " ".join(t for t in re.split(r"[ \t\r\n]+", text) if t)


def key(text):
    return hashlib.sha256(norm(text).encode("utf-8")).hexdigest()[:HASH_LEN]


def edges(text):
    body = text.strip(" \t\r\n")
    if not body:
        return "", ""
    start = text.index(body[0])
    return text[:start], text[start + len(body):]


def piece(text):
    """ModLang.Load keeps these raw, to translate parts of longer text."""
    return text != text.strip() or text.endswith((":", "("))


def scan(apk):
    found = set()
    for s in candidates(apk):
        found.add(key(s))
    return found


def pack(src, apk, dst):
    with open(src, encoding="utf-8") as fh:
        words = json.load(fh)
    present = scan(apk)
    entries, plain, dropped = {}, {}, []
    groups = collections.Counter(key(en) for en in words)
    for en, pt in sorted(words.items()):
        k = key(en)
        lead, trail = edges(en)
        # Lines that differ only by spacing ("Level", "Level ") share a hash and stay plain.
        # The mod only looks a line up by its folded form, except for pieces it matches inside
        # longer text; those must come back exactly as they were.
        exact = lead + norm(en) + trail == en
        if k in present and groups[k] == 1 and (exact or not piece(en)):
            entries[k] = pt if not (lead or trail) else [pt, lead, trail]
        elif len(norm(en).split()) <= 3:
            plain[en] = pt
        else:
            dropped.append(en)
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump({"format": f"sha256-{HASH_LEN} of ModLang.Norm(english)", "entries": entries, "plain": plain},
                  fh, ensure_ascii=False, indent=0, sort_keys=True)
        fh.write("\n")
    print(f"{len(entries)} hashed, {len(plain)} plain, {len(dropped)} left out")
    for en in dropped:
        print("  left out:", repr(en))


def assets(apk):
    """Each Unity data file of the APK, with .splitN parts joined."""
    with zipfile.ZipFile(apk) as z:
        parts = collections.defaultdict(list)
        for name in z.namelist():
            if not name.startswith(DATA) or name.endswith("/"):
                continue
            base = name[len(DATA):]
            m = re.match(r"(.+)\.split(\d+)$", base)
            if m:
                parts[m.group(1)].append((int(m.group(2)), name))
            else:
                parts[base].append((0, name))
        for base, files in parts.items():
            yield base, b"".join(z.read(n) for _, n in sorted(files))


RUN = re.compile(rb"[\x09\x0a\x0d\x20-\xff]+")


def serialized_strings(data):
    """Unity strings: a little-endian length right before the bytes. Found from printable runs."""
    for m in RUN.finditer(data):
        p = m.start()
        if p < 4:
            continue
        size = int.from_bytes(data[p - 4:p], "little")
        if 1 <= size <= m.end() - p:
            try:
                yield data[p:p + size].decode("utf-8")
            except UnicodeDecodeError:
                pass


def xml_text(data):
    text = data.decode("utf-8", "ignore")
    for m in re.finditer(r">([^<>]+)<", text):
        yield m.group(1)
    for m in re.finditer(r'="([^"]+)"', text):
        yield m.group(1)


def user_strings(dll):
    """The #US heap of a .NET assembly: every string literal of its code."""
    root = dll.find(b"BSJB")
    if root < 0:
        return
    vlen = int.from_bytes(dll[root + 12:root + 16], "little")
    p = root + 16 + vlen + 2
    count = int.from_bytes(dll[p:p + 2], "little")
    p += 2
    for _ in range(count):
        off = int.from_bytes(dll[p:p + 4], "little")
        size = int.from_bytes(dll[p + 4:p + 8], "little")
        end = dll.index(b"\0", p + 8)
        name = dll[p + 8:end]
        p = (end + 4) & ~3
        if name != b"#US":
            continue
        heap = dll[root + off:root + off + size]
        i = 1
        while i < len(heap):
            b = heap[i]
            if b & 0x80 == 0:
                ln, i = b, i + 1
            elif b & 0xC0 == 0x80:
                ln, i = ((b & 0x3F) << 8) | heap[i + 1], i + 2
            else:
                ln, i = ((b & 0x1F) << 24) | (heap[i + 1] << 16) | (heap[i + 2] << 8) | heap[i + 3], i + 4
            if ln > 1:
                yield heap[i:i + ln - 1].decode("utf-16-le", "ignore")
            i += ln


def candidates(apk):
    for name, data in assets(apk):
        if name.startswith("Managed/") and name.endswith(".dll"):
            yield from user_strings(data)
            continue
        if b"/>" in data or b"</" in data:
            yield from xml_text(data)
        if not name.endswith(".resS"):
            yield from serialized_strings(data)


def unpack(apk, src, dst):
    with open(src, encoding="utf-8") as fh:
        table = json.load(fh)
    entries = table["entries"]
    words = dict(table["plain"])
    found = set()
    for s in candidates(apk):
        k = key(s)
        if k in entries and k not in found:
            v = entries[k]
            pt, lead, trail = (v, "", "") if isinstance(v, str) else v
            words[lead + norm(s) + trail] = pt
            found.add(k)
    with open(dst, "w", encoding="utf-8") as fh:
        json.dump(words, fh, ensure_ascii=False, indent=0, sort_keys=True)
    missing = len(entries) - len(found)
    print(f"{len(found)} of {len(entries)} hashed lines found in this APK, {len(words)} translations"
          + (f"; {missing} not found, is this the 1.26 APK?" if missing else ""))
    if missing:
        raise SystemExit(1)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "pack":
        pack(*sys.argv[2:5])
    elif len(sys.argv) == 5 and sys.argv[1] == "unpack":
        unpack(*sys.argv[2:5])
    else:
        raise SystemExit(__doc__)
