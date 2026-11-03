#!/usr/bin/env python3
"""Lists the gamepad bindings of the controller GUI scene, for use in the touch scene.

Usage: gen_pad_map.py <merged asset dir> <decompiled firstpass dir> <out.txt>
The touch scene is the one the mod always loads, because it has every touch button. Its elements
have no gamepad bindings, so this reads them from the serialized StartGame_controller objects.
Each output line is: object name, field name, value.
"""
import collections
import os
import re
import struct
import sys

import UnityPy

CONTROL = re.compile(r"\bInputHandler\.ControllerInput\b")
SIZE = {"int": 4, "float": 4, "bool": 4, "byte": 4, "short": 4, "uint": 4, "long": 8,
        "Vector2": 8, "Vector3": 12, "Vector4": 16, "Quaternion": 16, "Color": 16, "Rect": 16}
ENUMS = set()


UNITY_ENUMS = {"TextAnchor", "TextAlignment", "KeyCode", "FontStyle", "Space", "WrapMode", "PlayMode",
               "AnimationBlendMode", "LayerMask", "RuntimePlatform", "QueueMode", "Mode", "AudioRolloffMode"}
UNITY_OBJECTS = {"GameObject", "Transform", "Texture", "Texture2D", "Material", "AudioClip", "AudioSource", "Camera",
                 "Font", "Mesh", "Shader", "Light", "Renderer", "MeshRenderer", "MeshFilter", "AnimationClip",
                 "Animation", "TextMesh", "GUITexture", "GUIText", "Rigidbody", "Collider", "BoxCollider",
                 "MonoBehaviour", "Component", "Object", "TextAsset", "ScriptableObject", "RenderTexture",
                 "ParticleSystem", "ParticleEmitter", "CharacterController", "TrailRenderer", "LineRenderer",
                 "SkinnedMeshRenderer", "Terrain", "Cubemap", "Behaviour", "GUIStyle"}


def parse_classes(code):
    classes = {}
    for path in os.listdir(code):
        if not path.endswith(".cs"):
            continue
        text = open(os.path.join(code, path), encoding="utf-8", errors="ignore").read()
        for m in re.finditer(r"\benum\s+(\w+)", text):
            ENUMS.add(m.group(1))
        m = re.search(r"(\[Serializable\]\s*)?public (?:abstract |sealed )?class (\w+)(?:\s*:\s*([\w.]+))?", text)
        if not m:
            continue
        fields = []
        depth = 0
        skip = False
        for line in text[m.end():].split("\n"):
            t = line.strip()
            if depth == 1:
                if t == "[NonSerialized]":
                    skip = True
                f = re.match(r"(\[SerializeField\]\s*)?(public\s+)?([\w.<>\[\], ]+?)\s+(\w+)(?:\s*=.*)?;$", t)
                if f and not re.search(r"\b(static|const|readonly|delegate|event|private|protected|return)\b", t.replace("[SerializeField]", "")) and (f.group(1) or f.group(2)):
                    if not skip:
                        fields.append((f.group(3), f.group(4)))
                    skip = False
            depth += line.count("{") - line.count("}")
        classes[m.group(2)] = (m.group(3), fields, bool(m.group(1)))
    return classes


def chain(classes, name):
    out = []
    while name in classes:
        base, fields, _ = classes[name]
        out = fields + out
        name = base
    return out


def is_object(classes, name):
    while name in classes:
        name = classes[name][0]
    return name in UNITY_OBJECTS


def walk(raw, pos, fields, classes):
    """Reads serialized fields in declaration order, as Unity 4 lays them out."""
    values = {}
    for ftype, fname in fields:
        short = ftype.split(".")[-1]
        if short in SIZE or short in ENUMS or short in UNITY_ENUMS:
            if short in ENUMS or short in UNITY_ENUMS or short in ("int", "bool"):
                values[fname] = struct.unpack("<i", raw[pos:pos + 4])[0]
            pos += SIZE.get(short, 4)
        elif short == "string":
            n = struct.unpack("<i", raw[pos:pos + 4])[0]
            pos += 4 + ((n + 3) & ~3)
        elif short.endswith("[]") or short.startswith("List<"):
            elem = short[:-2] if short.endswith("[]") else short[5:-1]
            elem = elem.split(".")[-1]
            n = struct.unpack("<i", raw[pos:pos + 4])[0]
            pos += 4
            if elem == "string":
                for _ in range(n):
                    k = struct.unpack("<i", raw[pos:pos + 4])[0]
                    pos += 4 + ((k + 3) & ~3)
            elif elem in SIZE or elem in ENUMS or elem in UNITY_ENUMS:
                pos += n * SIZE.get(elem, 4)
            elif elem in UNITY_OBJECTS or is_object(classes, elem):
                pos += n * 8
            elif elem in classes and classes[elem][2]:
                for _ in range(n):
                    _, pos = walk(raw, pos, chain(classes, elem), classes)
        elif short in UNITY_OBJECTS or is_object(classes, short):
            pos += 8
        elif short in classes and classes[short][2]:
            _, pos = walk(raw, pos, chain(classes, short), classes)
        # anything else (Dictionary, delegates, plain classes) is not serialized
    return values, pos


def script_names(paths):
    names = {}
    for p in paths:
        env = UnityPy.load(p)
        for o in env.objects:
            if o.type.name == "MonoScript":
                try:
                    names[o.path_id] = o.read().m_ClassName
                except Exception:
                    pass
    return names


def elements(path, scripts):
    env = UnityPy.load(path)
    names = {o.path_id: o.read().m_Name for o in env.objects if o.type.name == "GameObject"}
    for o in env.objects:
        if o.type.name != "MonoBehaviour":
            continue
        raw = o.get_raw_data()
        go = struct.unpack("<i", raw[4:8])[0]
        script = struct.unpack("<i", raw[16:20])[0]
        n = struct.unpack("<i", raw[20:24])[0]
        start = 24 + ((n + 3) & ~3)
        yield names.get(go, "?"), scripts.get(script, str(script)), raw, start


def main():
    base, code, out = sys.argv[1:4]
    classes = parse_classes(code)
    scripts = script_names([os.path.join(base, f) for f in ("sharedassets0.assets",) if os.path.exists(os.path.join(base, f))])
    print("scripts", len(scripts))
    wanted = {}
    for cls in classes:
        wanted[cls] = [f for f in chain(classes, cls) if f[0].endswith("ControllerInput")]
    lines = []
    bad = collections.Counter()
    for name, cls, raw, start in elements(os.path.join(base, "level2"), scripts):
        if cls not in classes:
            continue
        try:
            values, end = walk(raw, start, chain(classes, cls), classes)
        except struct.error:
            end = -1
        if end != len(raw):
            bad[cls] += 1
            continue
        for ftype, fname in wanted[cls]:
            v = values.get(fname, 0)
            if v:
                lines.append(f"{name}\t{cls}\t{fname}\t{v}")
        if values.get("hiddenIfController"):
            lines.append(f"{name}\t{cls}\thiddenIfController\t1")
    with open(out, "w") as fh:
        fh.write("\n".join(sorted(set(lines))) + "\n")
    touch = sorted({(n, c) for n, c, _, _ in elements(os.path.join(base, "level0"), scripts)})
    ctrl = {(n, c) for n, c, _, _ in elements(os.path.join(base, "level2"), scripts)}
    print("touch only:", [t for t in touch if t not in ctrl])
    print("controller only:", sorted(c for c in ctrl if c not in set(touch)))
    print(len(lines), "bindings; layouts that did not fit:", dict(bad))


if __name__ == "__main__":
    main()
