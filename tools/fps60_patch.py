"""The 60 fps mode, patched into the recompiled code (work/gen, work/gen_ovl)
behind a switch.

    python tools/fps60_patch.py work/gen work/gen_ovl      (setup.ps1 runs it)

This is the "60 FPS" cheat for ULES-01213 from the CWCheat Database Plus (the
database PPSSPP uses), turned into C: cheats write the game's code, and here
the code is recompiled, so each write becomes a choice the generated code
makes at run time through mhfu_fps60() (host/lobby.c: the player's choice,
capped by the room's). Its "60 FPS [Disable]" block holds the original words;
they match this EBOOT (the PSN one).

What it does:
  - frames start every vblank instead of every second one, unless the game's
    flag at 0x08B59914 is set (the vblank interrupt, 0x088591A4);
  - the time step 2.0 (two vblanks a frame) becomes 1.0, and a few speeds and
    durations follow (constants in the EBOOT and in lobby_task / game_task);
  - durations counted in frames double (slti 30/60/80, 600, 90);
  - four counters that move every frame move every other frame (the cheat's
    code caves: a toggle per counter, mhfu_fps_tog()).
A table of floats in game_task's data (0x09B07C98) is not code: the host
writes it at run time (host/lobby.c, mhfu_fps60_data()).

A patch applies only where the generated code still holds the original
instruction (the cheat's own checks): overlays that share a slot are told apart
that way, by group. Idempotent: a file already patched is left alone.
"""
import glob
import os
import re
import sys

MARK = "/* mhfu fps60 */"
PROTO = MARK + "\nint mhfu_fps60(void);\nunsigned mhfu_fps_tog(int which);\n"

# A constant: (address, mnemonic, operands as the generated comment has them, old, new).
# For lui the values are the upper halves; for addiu/slti the immediates.
def lui(addr, reg, old, new):
    return ("const", addr, "lui", f"${reg}, 0x{old:X}", f"0x{old:04X}0000u", f"0x{new:04X}0000u")


def imm(addr, mn, ops, old, new):
    return ("const", addr, mn, ops, str(old), str(new))


EBOOT = [
    # frames every vblank, unless the game's flag says otherwise (the cave at 0x0880202C)
    ("replace", 0x088591A4, "slti", "$v1, $a0, 2",
     "r_v1 = psp_slt(r_a0, (uint32_t)2);",
     "r_v1 = (mhfu_fps60() && psp_read32(0x08B59914u) == 0) ? 0u : psp_slt(r_a0, (uint32_t)2);"),
    # a counter at +0x10 counts down every other frame (the cave at 0x08802000)
    ("after", 0x0886DFCC, "subu", "$v1, $v1, $a1",
     "if (mhfu_fps60() && mhfu_fps_tog(0) == 0) r_v1 = r_v1 + r_a1;"),
    lui(0x088D5DF8, "v0", 0x4000, 0x3F80),
    imm(0x0888749C, "addiu", "$v1, $zero, -1150", -1150, -575),
    imm(0x088874A0, "addiu", "$v1, $zero, 1150", 1150, 575),
    # 2.0, or 1.0 unless the float at 0x090AF21C is 500 (the cave at 0x088020B0)
    ("replace", 0x088D5E1C, "lui", "$v0, 0x4000",
     "r_v0 = 0x40000000u;",
     "r_v0 = (mhfu_fps60() && psp_read32(0x090AF21Cu) != 0x43FA0000u) ? 0x3F800000u : 0x40000000u;"),
    imm(0x088D1C3C, "slti", "$at, $v0, 30", 30, 60),
    imm(0x088D1C44, "slti", "$at, $v0, 60", 60, 120),
    imm(0x088D1C54, "slti", "$at, $v0, 80", 80, 160),
    imm(0x0886B3B4, "addiu", "$v0, $zero, 600", 600, 1200),
]

# Overlay groups: the first patch is the cheat's check for the right overlay.
OVERLAY = [
    [lui(0x09AAEDD0, "v0", 0x4000, 0x3F80), lui(0x09AAB030, "v1", 0x4020, 0x3FA0), lui(0x09AAB044, "v1", 0x4020, 0x3FA0),
     lui(0x09AAF030, "v1", 0x4000, 0x3F80), lui(0x09ABC4C8, "v0", 0x4000, 0x3F80)],
    [lui(0x09AA31F0, "v1", 0x4000, 0x3F80), lui(0x09AA31CC, "v1", 0x4000, 0x3F80), lui(0x09AA4ACC, "v1", 0x4000, 0x3F80),
     lui(0x09AA31F4, "v1", 0x4059, 0x3FD9), lui(0x09AA4AD0, "v1", 0x4059, 0x3FD9), lui(0x09A65FB0, "v0", 0x4000, 0x3F80),
     lui(0x09A6767C, "v1", 0x4000, 0x3F80), lui(0x09A9E208, "v0", 0x4020, 0x3FA0), lui(0x09A9E234, "v0", 0x4020, 0x3FA0),
     lui(0x09A9E260, "v0", 0x4020, 0x3FA0), lui(0x09A6867C, "v1", 0x4000, 0x3F80), lui(0x09A759D4, "v0", 0x4000, 0x3F80),
     lui(0x09A759B4, "v0", 0x4000, 0x3F80), lui(0x09A7599C, "v0", 0x4000, 0x3F80)],
    [lui(0x09AC6698, "v1", 0x4000, 0x3F80), lui(0x09AC6964, "v0", 0x4000, 0x3F80), lui(0x09AC6AF4, "v0", 0x4000, 0x3F80),
     lui(0x09ACB7B8, "v0", 0x3F80, 0x3F00), lui(0x09ACB7E0, "v0", 0x4000, 0x3F80)],
    # a step added every frame, when small, is added every other frame (the cave at 0x0880204C)
    [("after", 0x09A6A22C, "addu", "$v1, $v1, $s0",
      "if (mhfu_fps60() && (r_s0 < 4u || r_s0 >= 0xFFFFFFFCu) && mhfu_fps_tog(1) == 0) r_v1 = r_v1 - r_s0;")],
    # a counter counts up every other frame (the cave at 0x0880208C)
    [("before", 0x09AA25CC, "addiu", "$v0, $zero, 866", None),
     ("before", 0x09AA25D4, "sh", "$v1, 1688($s4)",
      "if (mhfu_fps60() && mhfu_fps_tog(2) == 0) r_v1 = r_v1 - 1;")],
    [lui(0x09AB235C, "v1", 0x400C, 0x3F8C), lui(0x09AB24DC, "v1", 0x400C, 0x3F8C), lui(0x09AB25B0, "v0", 0x4000, 0x3F80),
     lui(0x09AB31E8, "v0", 0x4000, 0x3F80), lui(0x09AB34DC, "a0", 0x4000, 0x3F80)],
    [lui(0x09A9ECFC, "v0", 0x3FCC, 0x3F4C), lui(0x09A9ED10, "v0", 0x4020, 0x3FA0)],
    [imm(0x09BA1FC4, "addiu", "$v0, $zero, 90", 90, 180)],
    # a counter counts down every other frame (the cave at 0x088020D4)
    [("before", 0x09ADD930, "blez", "$v0, 0x09ADD948",
      "if (mhfu_fps60() && mhfu_fps_tog(3) == 0) r_v0 = r_v0 + 1;")],
]

COMMENT = re.compile(r"^\s*/\* ([0-9A-F]{8})  (\S+)\s+(.*?) \*/\s*$")


def norm(s):
    return re.sub(r"\s+", " ", s.strip())


def index(lines):
    """Address -> line number of its comment."""
    at = {}
    for i, l in enumerate(lines):
        m = COMMENT.match(l)
        if m:
            at[int(m.group(1), 16)] = (i, m.group(2), norm(m.group(3)))
    return at


def matches(at, p):
    e = at.get(p[1])
    return e is not None and e[1] == p[2] and e[2] == norm(p[3])


def apply(lines, at, p):
    """One patch into `lines` (indices from `at`, applied last-first by the caller)."""
    i = at[p[1]][0]
    kind = p[0]
    if kind == "const":
        old, new = p[4], p[5]
        stmt = lines[i + 1]
        if p[2] == "lui":
            assert old in stmt, (hex(p[1]), stmt)
            lines[i + 1] = stmt.replace(old, f"(mhfu_fps60() ? {new} : {old})", 1) + "   " + MARK
        elif p[2] == "slti":
            assert f"(uint32_t){old})" in stmt, (hex(p[1]), stmt)
            lines[i + 1] = stmt.replace(f"(uint32_t){old})", f"(uint32_t)(mhfu_fps60() ? {new} : {old}))", 1) + "   " + MARK
        else:  # addiu rX, $zero, n -> r_x = r_zero + n;
            assert stmt.rstrip().endswith(f"+ {old};"), (hex(p[1]), stmt)
            lines[i + 1] = stmt.rstrip()[:-len(f"{old};")] + f"(mhfu_fps60() ? {new} : {old});   " + MARK
    elif kind == "replace":
        assert p[4] in lines[i + 1], (hex(p[1]), lines[i + 1])
        lines[i + 1] = lines[i + 1].replace(p[4], p[5], 1) + "   " + MARK
    elif kind == "after":
        indent = re.match(r"\s*", lines[i + 1]).group(0)
        lines.insert(i + 2, indent + p[4] + "   " + MARK)
    elif kind == "before":
        if p[4]:
            indent = re.match(r"\s*", lines[i]).group(0)
            lines.insert(i, indent + p[4] + "   " + MARK)


def read(path):
    return open(path, encoding="utf-8", errors="surrogateescape").read()


def write(path, s):
    open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)


def revert(path):
    """This script's patches out of one file: the constants back, its lines gone."""
    s = read(path)
    if MARK not in s:
        return False
    if s.startswith(PROTO):
        s = s[len(PROTO):]
    out = []
    replaced = {p[5]: p[4] for p in EBOOT if p[0] == "replace"}
    for l in s.split("\n"):
        if MARK not in l:
            out.append(l)
            continue
        l = l.replace("   " + MARK, "")
        if l.strip().startswith("if (mhfu_fps60()"):
            continue                                       # a line this script inserted
        l = re.sub(r"\(mhfu_fps60\(\) \? [^:]+ : ([^)]+)\)", r"\1", l)
        for new, old in replaced.items():
            l = l.replace(new, old)
        out.append(l)
    write(path, "\n".join(out))
    return True


def patch_unit(files, groups):
    """One unit of recompiled code (the EBOOT, or one overlay) spread over `files`:
    each group whose first instruction is there (the cheat's check) is applied
    wherever its instructions are. Returns how many patches applied."""
    texts = {f: read(f) for f in files}
    if any(MARK in t for t in texts.values()):
        return -1
    lines = {f: t.split("\n") for f, t in texts.items()}
    at = {}
    for f, ls in lines.items():
        for a, e in index(ls).items():
            at[a] = (f,) + e
    def ok(p):
        e = at.get(p[1])
        return e is not None and e[2] == p[2] and e[3] == norm(p[3])
    todo = {}
    for group in groups:
        if not ok(group[0]):
            continue                                       # not in this unit (the cheat's check)
        for p in group:
            if ok(p):
                todo.setdefault(at[p[1]][0], []).append(p)
            else:
                print(f"fps60: {p[1]:08X} is not '{p[2]} {p[3]}' here, left alone")
    n = 0
    for f, ps in todo.items():
        ls = lines[f]
        local = index(ls)
        for p in sorted(ps, key=lambda p: local[p[1]][0], reverse=True):
            apply(ls, local, p)
            n += 1
        write(f, PROTO + "\n".join(ls))
    return n


def main():
    args = sys.argv[1:]
    undo = "--revert" in args
    dirs = [a for a in args if a != "--revert"] or [os.path.join("work", "gen"), os.path.join("work", "gen_ovl")]
    units = []                                             # (name, files, groups)
    for d in dirs:
        top = sorted(glob.glob(os.path.join(d, "*.c")))
        if top:
            units.append((d, top, [[p] for p in EBOOT]))
        for sub in sorted(glob.glob(os.path.join(d, "*", ""))):
            fs = sorted(glob.glob(os.path.join(sub, "*.c")))
            if fs:
                units.append((os.path.basename(os.path.dirname(sub)), fs, OVERLAY))
    total = 0
    for name, files, groups in units:
        if undo:
            n = sum(revert(f) for f in files)
            if n:
                print(f"fps60: reverted {n} file(s) of {name}")
            continue
        n = patch_unit(files, groups)
        if n > 0:
            print(f"fps60: {n} patch(es) in {name}")
            total += n
        elif n < 0:
            print(f"fps60: {name} was patched already")
    if not undo:
        print(f"fps60: {total} patch(es) applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
