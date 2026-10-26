"""Built-in mods, patched into the recompiled code behind switches the host
flips at run time (host/mods.c).

    python tools/mods_patch.py work/gen work/gen_ovl          (setup.ps1 runs it)
    python tools/mods_patch.py --revert work/gen work/gen_ovl

Cheats that write the game's code cannot work on recompiled code, so a mod of
that kind is turned into C here, the way tools/fps60_patch.py does the 60 fps
mode.

  - Widescreen: the camera's aspect, 480/272 as a float (lui/ori 0x3FE1E1E2,
    stored at +8 of the camera at 0x08812AC4 and 0x08812BBC), becomes the
    window's when the mod is on (mhfu_mod_aspect), and the renderer stretches
    the picture over the window. The CWCheat Database Plus "Screen 16:10 ..."
    codes for ULES-01213 patch the first of these, 0xEB8 further on in the
    build they were made for (0x0881397C).

  - The code mods in mods/builtin (tools/mods_convert.py): at each hooked
    instruction the recompiled game checks for the mod's word in memory and,
    when it is there, does what the hooked game would: a `j` runs its delay
    slot and goes to the mod's (recompiled) code; another word is used as the
    instruction. The host writes and removes the words (host/mods.c), whose
    data this script also writes, into work/gen_ovl/mods_builtin.c.

A patch applies only where the generated comment still shows the original
instruction. Idempotent: a file already patched is left alone.
"""
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MARK = "/* mhfu mods */"
PROTO = MARK + "\nunsigned mhfu_mod_aspect(unsigned original);\n"
COMMENT = re.compile(r"\s*/\* ([0-9A-F]{8})  (\S+)\s+(.*?) \*/\s*$")

# Widescreen: (address, mnemonic, operands, the C line it compiles to, a line to add after it)
WIDESCREEN = [
    (0x08812AC8, "ori", "$v0, $v0, 0xE1E2", "r_v0 = r_v0 | 0xE1E2u;", "r_v0 = mhfu_mod_aspect(r_v0);"),
    (0x08812BC0, "ori", "$v0, $v0, 0xE1E2", "r_v0 = r_v0 | 0xE1E2u;", "r_v0 = mhfu_mod_aspect(r_v0);"),
]
# Instructions whose C is not a plain statement: never copied as a delay slot.
BRANCHES = {"beq", "bne", "beql", "bnel", "blez", "bgtz", "blezl", "bgtzl", "bltz", "bgez", "bltzl", "bgezl",
            "bltzal", "bgezal", "j", "jal", "jr", "jalr", "bc1t", "bc1f", "bc1tl", "bc1fl", "bvt", "bvf", "bvtl", "bvfl",
            "syscall", "break"}


def norm(s):
    return re.sub(r"\s+", " ", s.strip())


def read(path):
    return open(path, encoding="utf-8", errors="surrogateescape").read()


def write(path, s):
    open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)


def load_mods():
    p = os.path.join(REPO, "mods", "builtin", "mods.json")
    return json.load(open(p)) if os.path.exists(p) else []


def delay_lines(lines, i):
    """The C of the instruction whose comment is lines[i] (a delay slot), or None if it is not plain."""
    m = COMMENT.match(lines[i])
    if not m or m.group(2) in BRANCHES:
        return None
    out = []
    for l in lines[i + 1:]:
        t = l.strip()
        if not t or t.startswith(("/*", "L_", "if (", "{", "}", "PSP_", "return", "goto", "psp_func", "psp_dispatch")) \
                or re.match(r"r_ra = 0x[0-9A-F]+u;", t):
            break
        out.append(t)
    return out


def patch_file(path, hooks):
    """The widescreen lines and the mods' hooks into one file; how many applied."""
    s = read(path)
    if MARK in s:
        return -1
    lines = s.split("\n")
    at = {}
    for i, l in enumerate(lines):
        m = COMMENT.match(l)
        if m:
            at[int(m.group(1), 16)] = (i, m.group(2), norm(m.group(3)))
    edits = []                                       # (line index, "before"/"after"/"replace", text)
    for addr, mn, ops, c, add in WIDESCREEN:
        e = at.get(addr)
        if e and e[1] == mn and e[2] == norm(ops) and c in lines[e[0] + 1]:
            edits.append((e[0] + 1, "after", add))
    for h in hooks:
        e = at.get(h["addr"])
        if not e or e[1] != h["mnemonic"]:
            continue
        i = e[0]
        if h["kind"] == "j":
            if "delay" in h:
                delay = []                           # the hook replaced its delay slot too (a nop)
            else:
                nxt = at.get(h["addr"] + 4)
                delay = delay_lines(lines, nxt[0]) if nxt else None
                if delay is None:
                    delay = []                       # a branch in the delay slot: the mod returns to it
            body = " ".join(delay + [f"psp_dispatch(0x{h['target']:08X}u); return;"])
            edits.append((i, "before", f"if (psp_read32(0x{h['addr']:08X}u) == 0x{h['word']:08X}u) {{ {body} }}"))
        elif h["kind"] == "word" and not h.get("companion"):
            # addiu rt, $zero, imm -> the mod's immediate while its word is there
            w, o = h["word"], h["orig"]
            if w >> 26 != 0x09 or o >> 26 != 0x09 or (w >> 21) & 31 or (o >> 21) & 31 or (w >> 16) & 31 != (o >> 16) & 31:
                print(f"mods: {h['addr']:08X}: only `addiu rt, $zero, n` words are handled")
                continue
            imm_o, imm_w = o & 0xFFFF, w & 0xFFFF
            stmt = lines[i + 1]
            want = f"+ {imm_o - 0x10000 if imm_o & 0x8000 else imm_o};"
            if not stmt.rstrip().endswith(want):
                print(f"mods: {h['addr']:08X}: unexpected C '{stmt.strip()}'")
                continue
            reg = stmt.strip().split(" =")[0]
            v = imm_w - 0x10000 if imm_w & 0x8000 else imm_w
            edits.append((i + 1, "replace",
                          f"{reg} = psp_read32(0x{h['addr']:08X}u) == 0x{w:08X}u ? (uint32_t){v} : {stmt.strip().split('= ', 1)[1].rstrip(';')};"))
    if not edits:
        return 0
    for i, how, text in sorted(edits, reverse=True):
        indent = re.match(r"\s*", lines[i]).group(0)
        if how == "after":
            lines.insert(i + 1, indent + text + "   " + MARK)
        elif how == "before":
            lines.insert(i, indent + text + "   " + MARK)
        else:
            lines[i] = indent + text + "   " + MARK + "  /* was: " + lines[i].strip().replace("*/", "* /") + " */"
    write(path, PROTO + "\n".join(lines))
    return len(edits)


def revert(path):
    s = read(path)
    if MARK not in s:
        return False
    if s.startswith(PROTO):
        s = s[len(PROTO):]
    out = []
    for l in s.split("\n"):
        if MARK not in l:
            out.append(l)
        elif "/* was: " in l:
            indent = re.match(r"\s*", l).group(0)
            out.append(indent + l.split("/* was: ", 1)[1].rsplit(" */", 1)[0].replace("* /", "*/"))
    write(path, "\n".join(out))
    return True


def write_table(ovl_dir, mods):
    """work/gen_ovl/mods_builtin.c: each mod's words and hooks for the host."""
    def cstr(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
    out = ["/* Written by tools/mods_patch.py from mods/builtin -- do not edit. */",
           '#include "mods_builtin.h"', ""]
    rows = []
    for m in mods:
        d = open(os.path.join(REPO, "mods", "builtin", m["id"] + ".bin"), "rb").read()[0x40:]
        words = struct.unpack("<%dI" % (len(d) // 4), d)
        out.append(f"static const uint32_t words_{m['id']}[] = {{")
        for k in range(0, len(words), 8):
            out.append("    " + ", ".join(f"0x{w:08X}u" for w in words[k:k + 8]) + ",")
        out.append("};")
        out.append(f"static const mhfu_mod_hook hooks_{m['id']}[] = {{")
        for h in m["hooks"]:
            out.append(f"    {{ 0x{h['addr']:08X}u, 0x{h['orig']:08X}u, 0x{h['word']:08X}u, {1 if h['unit'] == 'eboot' else 0} }},"
                       f"  /* {h['unit']} {h['mnemonic']} */")
        out.append("};")
        rows.append(f"    {{ {cstr(m['id'])}, {cstr(m['name'])}, {cstr(m['description'])}, {1 if m['default'] else 0}, "
                    f"0x{m['base']:08X}u, {len(words)}u, words_{m['id']}, hooks_{m['id']}, {len(m['hooks'])}, 0x{m['overlay']}u }},")
    out.append("")
    out.append("const mhfu_builtin_mod mhfu_builtin_mods[] = {")
    out += rows if rows else ["    {0},"]
    out.append("};")
    out.append(f"const int mhfu_nbuiltin_mods = {len(rows)};")
    open(os.path.join(ovl_dir, "mods_builtin.c"), "w", newline="\n").write("\n".join(out) + "\n")
    print(f"mods: {len(rows)} built-in mod(s) in {os.path.join(ovl_dir, 'mods_builtin.c')}")


def main():
    args = sys.argv[1:]
    undo = "--revert" in args
    dirs = [a for a in args if a != "--revert"] or [os.path.join("work", "gen"), os.path.join("work", "gen_ovl")]
    mods = load_mods()
    hooks = [h for m in mods for h in m["hooks"]]
    total = 0
    for d in dirs:
        files = sorted(glob.glob(os.path.join(d, "*.c")) + glob.glob(os.path.join(d, "*", "*.c")))
        for f in files:
            if os.path.basename(f) in ("overlays.c", "mods_builtin.c"):
                continue
            if undo:
                if revert(f):
                    print(f"mods: reverted {f}")
                continue
            n = patch_file(f, hooks)
            if n > 0:
                total += n
            elif n < 0:
                print(f"mods: {f} was patched already")
    if not undo:
        print(f"mods: {total} patch(es) applied")
        if len(dirs) > 1:
            write_table(dirs[1], mods)
    return 0


if __name__ == "__main__":
    sys.exit(main())
