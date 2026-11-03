#!/usr/bin/env python3
"""Generate bridge/gen/gl_table.inc: one thunk per GL function that libunity imports.

Usage: gen_gl_table.py <libunity imports, one per line> <sysroot include dir> <out file>
"""
import re
import sys


def main() -> None:
    imports_path, include, out = sys.argv[1:4]
    with open(imports_path, encoding="utf-8") as fh:
        imports = {line.strip() for line in fh if line.strip().startswith("gl")}
    known = set()
    for header in ("GLES2/gl2.h", "GLES/gl.h"):
        with open(f"{include}/{header}", encoding="utf-8") as fh:
            known |= set(re.findall(r"\b(gl[A-Z]\w+)\s*\(", fh.read()))
    custom = {"glGetString", "glShaderSource", "glGetVertexAttribPointerv",
              # bridge/src/fx.cpp redirects these to the remaster render targets.
              "glBindFramebuffer", "glViewport", "glScissor", "glGetIntegerv",
              "glTexParameteri", "glTexParameterf", "glCopyTexSubImage2D"}
    found = sorted((imports & known) - custom)
    missing = sorted(imports - known - custom)
    with open(out, "w", encoding="utf-8") as fh:
        for name in found:
            fh.write(f"T({name}),\n")
    print(f"{len(found)} GL thunks, {len(missing)} without a header prototype: {missing}")


if __name__ == "__main__":
    main()
