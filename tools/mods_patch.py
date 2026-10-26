"""Built-in mods, patched into the recompiled code (work/gen) behind switches
the host flips at run time (host/mods.c).

    python tools/mods_patch.py work/gen          (setup.ps1 runs it)
    python tools/mods_patch.py --revert work/gen

Cheats that write the game's code cannot work on recompiled code, so a mod of
that kind is turned into C here, the way tools/fps60_patch.py does the 60 fps
mode. So far:

  - Widescreen: the camera's aspect, 480/272 as a float (lui/ori 0x3FE1E1E2,
    stored at +8 of the camera at 0x08812AC4 and 0x08812BBC), becomes the
    window's when the mod is on (mhfu_mod_aspect), and the renderer stretches
    the picture over the window. The CWCheat Database Plus "Screen 16:10 ..."
    codes for ULES-01213 patch the first of these, 0xEB8 further on in the
    build they were made for (0x0881397C).

A patch applies only where the generated comment still shows the original
instruction. Idempotent: a file already patched is left alone.
"""
import glob
import os
import re
import sys

MARK = "/* mhfu mods */"
PROTO = MARK + "\nunsigned mhfu_mod_aspect(unsigned original);\n"
COMMENT = re.compile(r"\s*/\* ([0-9A-F]{8})  (\S+)\s+(.*?) \*/\s*$")

# (address, mnemonic, operands, the C line it compiles to, a line to add after it)
PATCHES = [
    (0x08812AC8, "ori", "$v0, $v0, 0xE1E2", "r_v0 = r_v0 | 0xE1E2u;", "r_v0 = mhfu_mod_aspect(r_v0);"),
    (0x08812BC0, "ori", "$v0, $v0, 0xE1E2", "r_v0 = r_v0 | 0xE1E2u;", "r_v0 = mhfu_mod_aspect(r_v0);"),
]


def norm(s):
    return re.sub(r"\s+", " ", s.strip())


def read(path):
    return open(path, encoding="utf-8", errors="surrogateescape").read()


def write(path, s):
    open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)


def patch(path):
    s = read(path)
    if MARK in s:
        return -1
    lines = s.split("\n")
    todo = []
    for i, l in enumerate(lines):
        m = COMMENT.match(l)
        if not m:
            continue
        addr = int(m.group(1), 16)
        for p in PATCHES:
            if p[0] == addr:
                if m.group(2) == p[1] and norm(m.group(3)) == norm(p[2]) and i + 1 < len(lines) and p[3] in lines[i + 1]:
                    todo.append((i + 1, p))
                else:
                    print(f"mods: {addr:08X} is not '{p[1]} {p[2]}' here, left alone")
    if not todo:
        return 0
    for at, p in sorted(todo, reverse=True):
        indent = re.match(r"\s*", lines[at]).group(0)
        lines.insert(at + 1, indent + p[4] + "   " + MARK)
    write(path, PROTO + "\n".join(lines))
    return len(todo)


def revert(path):
    s = read(path)
    if MARK not in s:
        return False
    if s.startswith(PROTO):
        s = s[len(PROTO):]
    write(path, "\n".join(l for l in s.split("\n") if MARK not in l))
    return True


def main():
    args = sys.argv[1:]
    undo = "--revert" in args
    dirs = [a for a in args if a != "--revert"] or [os.path.join("work", "gen")]
    total = 0
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, "*.c"))):
            if undo:
                if revert(f):
                    print(f"mods: reverted {f}")
                continue
            n = patch(f)
            if n > 0:
                total += n
            elif n < 0:
                print(f"mods: {f} was patched already")
    if not undo:
        print(f"mods: {total} patch(es) applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
