"""Turn the US (ULUS-10391) code mods into built-in mods for the EU build.

    python tools/mods_convert.py <folder with the mods> [EU EBOOT] [US EBOOT]

The folder holds one subfolder per mod with its ULUS-10391.TXT (CWCheat). Writes
mods/builtin/<id>.bin (the mod's code and data, as an "MWo3" overlay that setup
recompiles like the game's own) and mods/builtin/mods.json (what the host and
tools/mods_patch.py need: the hooks, their original words, where the code lives).

These mods inject MIPS functions into free memory and hook the game's code with
`j`. Here the game is recompiled, so:
  - every address is moved from the US build to the EU one (see us2eu: the code
    around the hooks is the same, the data, heap and overlays moved by a few
    hundred bytes; every shift below was checked against the EU game, with a
    memory dump from a quest);
  - each mod is moved to its own 64 KB-aligned place (0x09E1xxxx-0x09E4xxxx,
    memory the game never uses and the host reserves), since all three were
    written for the same free space and overlap there; a 64 KB step keeps
    every lui/lo pair valid with only the lui changed;
  - its code is recompiled (setup.ps1, like an overlay) and the hooks become a
    check the recompiled game makes at each hooked instruction (mods_patch.py).
"""
import glob
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The mods: folder name prefix -> (id, name, description, relocation delta)
MODS = [
    ("Target Camera", "targetcam", "Target Camera + Crosshair",
     "Community mod: Target Camera Mod v0.7, with its crosshair.", 0x01500000),
    ("Damage Numbers", "damage", "Damage Numbers",
     "Community mod: Damage Numbers Display v0.4.", 0x01510000),
    ("Gathering Nodes", "gathering", "Gathering Node Indicators",
     "Community mod: Gathering Nodes Mod v0.4.", 0x01520000),
]
OWN_LO, OWN_HI = 0x0891F000, 0x08922000          # where the US mods put their code and data

# Overlays load 0x100 lower in the EU build (the slot table: 0x09A5F200 against
# 0x09A5F300) and their code is the same: every overlay address the mods use
# was found at -0x100 in the EU game_task / game_sub (sh v0,740(s5) at
# 0x09AC79D4, addiu v0,zero,6 at 0x09B68954, addiu v1,zero,1 at 0x09C447DC, a
# function at 0x09A6F390). tools/mods_patch.py checks each hooked one again.
OVERLAY_CODE = {}
# Scratch the gathering mod passes to that function: high in memory (a thread's
# stack on a PSP); here inside the mod's own space instead.
SCRATCH = {0x09FFF4A0: 0x3E00, 0x09FFF4BE: 0x3E1E}
# Loads and stores through a register the game set before the hook: its value there.
BASES = {0x0891F9FC: 0x08A60000}                  # target cam: v1 = lui 0x08A6 at 0x08869410


def load_elf(p):
    d = open(p, "rb").read()
    phoff, = struct.unpack_from('<I', d, 0x1C)
    t, o, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', d, phoff)
    mem = bytearray(ms)
    mem[:fs] = d[o:o + fs]
    return va, mem


def us2eu(a):
    """A US address the mods use, in the EU build."""
    if a in OVERLAY_CODE:
        return OVERLAY_CODE[a]
    if 0x08804000 <= a < 0x0890DF58:
        return a + text_delta(a)                      # the code: the same, checked around it
    if 0x0890DF58 <= a < 0x08C00000:
        return a - 0x120                              # stubs, .data, .bss
    if 0x08C00000 <= a < 0x09A5F300:
        return a - 0x140                              # the heap (player, stage, textures)
    if 0x09C10000 <= a < 0x09C19100:
        return a - 0x100                              # game_task's bss (the large monster list)
    if 0x09A5F300 <= a < 0x09D63100:
        return a - 0x100                              # overlay code (slots 1-3)
    if a >= 0x09FC0000 or 0x04000000 <= a < 0x04800000:
        return a                                      # allocated from the top / VRAM: the same
    raise ValueError(f"{a:08X}: no rule")


_text = {}


def text_delta(a):
    if a in _text:
        return _text[a]

    def mask(w):
        op = w >> 26
        return w & 0xFC000000 if op in (2, 3) else (w if op == 0 else w & 0xFFFF0000)

    def win(mem, va, x):
        return [mask(struct.unpack_from("<I", mem, x + 4 * k - va)[0]) for k in range(-6, 6)]
    w = win(US[1], US[0], a)
    hits = [d for d in range(-0x400, 0x404, 4) if win(EU[1], EU[0], a + d) == w]
    if len(hits) != 1:
        raise ValueError(f"{a:08X}: {len(hits)} matches in the EU code")
    _text[a] = hits[0]
    return hits[0]


def parse(p):
    cheats = []
    for l in open(p, encoding="utf-8", errors="replace"):
        l = l.strip()
        if l.startswith("_C"):
            cheats.append([l[4:].strip(), []])
        elif l.startswith("_L") and cheats:
            a, b = l[2:].split()[:2]
            cheats[-1][1].append((int(a, 16), int(b, 16)))
    return cheats


def sext(x):
    return x - 0x10000 if x & 0x8000 else x


LO_OPS = {0x09, 0x0D, 0x23, 0x2B, 0x21, 0x25, 0x20, 0x24, 0x28, 0x29, 0x31, 0x39, 0x35, 0x3D, 0x32, 0x3A, 0x36, 0x3E}


def convert(folder, mid, name, desc, delta):
    cheats = parse(os.path.join(folder, "ULUS-10391.TXT"))
    mem = {}                                         # byte address -> byte, the mod's own space
    hooks = []                                       # (US address, word)
    for cname, lines in cheats:
        for a, b in lines:
            t, off = a >> 28, 0x08800000 + (a & 0x0FFFFFFF)
            n = {0: 1, 1: 2, 2: 4}[t]
            if t == 0 and n == 1 and b > 0xFF:
                n = 2 if b <= 0xFFFF else 4
            if OWN_LO <= off < OWN_HI:
                for k in range(n):
                    mem[off + k] = (b >> (8 * k)) & 0xFF
            elif 0x08804000 <= off < 0x0890DF58 and t == 2:
                hooks.append((off, b))
            else:
                raise ValueError(f"{mid}: a write to {off:08X}")
    lo = min(mem) & ~3
    hi = (max(mem) + 4) & ~3

    def w32(a):
        return sum(mem.get(a + k, 0) << (8 * k) for k in range(4))

    def own(a):
        return OWN_LO <= a < OWN_HI

    def reloc(a):
        if own(a):
            return a + delta
        if a in SCRATCH:
            return lo + delta - (lo & 0xFFFF) + SCRATCH[a]
        return us2eu(a)

    # ---- which words are code: everything reached from the entries ----
    entries = set()
    for a, w in hooks:
        if w >> 26 == 2:
            entries.add(((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
    # the overlay hooks the damage mod writes at run time (lui/addiu t1 = a `j` word)
    for a in range(lo, hi, 4):
        w = w32(a)
        if w >> 16 == 0x3C09 and w32(a + 4) >> 16 == 0x2529:
            word = (((w & 0xFFFF) << 16) + sext(w32(a + 4) & 0xFFFF)) & 0xFFFFFFFF
            t = ((a + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if word >> 26 == 2 and OWN_LO <= t < OWN_HI:
                entries.add(t)
    code = set()
    walked = set()           # not `code`: a delay slot is code before anything walks on from it
    todo = sorted(entries)
    while todo:
        pc = todo.pop()
        while own(pc) and pc not in walked and lo <= pc < hi:
            walked.add(pc)
            code.add(pc)
            w = w32(pc)
            op = w >> 26
            if op == 3 or (op == 0 and w & 0x3F == 0x09):        # jal / jalr: back after the call
                if op == 3:
                    t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if own(t):
                        todo.append(t)
                pc += 4
                continue
            if op == 2:                                           # j
                t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                code.add(pc + 4)
                if own(t):
                    todo.append(t)
                break
            if op == 0 and w & 0x3F == 0x08:                      # jr
                code.add(pc + 4)
                break
            branch = op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 0x10, 0x11)) \
                or (op == 0x12 and (w >> 21) & 0x1F == 8)        # COP2 (VFPU) bvf/bvt
            if branch:
                t = pc + 4 + (sext(w & 0xFFFF) << 2)
                todo.append(t)
                if op == 4 and (w >> 16) & 0x3FF == 0:            # beq zero,zero: always
                    code.add(pc + 4)
                    break
            pc += 4

    # ---- rewrite ----
    out = {a: w32(a) for a in range(lo, hi, 4)}
    notes = []
    us_hook_words = {}
    for a, _ in hooks:                     # the instructions the hooks replace, copied into the mods
        for k in (0, 4):
            us_hook_words[struct.unpack_from("<I", US[1], a + k - US[0])[0]] = struct.unpack_from("<I", EU[1], us2eu(a) + k - EU[0])[0]
    regs = {}                              # register -> (lui pc, value) while it holds a lui
    pending = {}                           # lui pc -> list of (use pc, full address, ori?)
    for pc in range(lo, hi, 4):
        w = out[pc]
        if pc not in code:
            continue
        op = w >> 26
        if pc in entries:
            regs = {}
        if pc in BASES:
            imm = sext(w & 0xFFFF)
            n = reloc((BASES[pc] + imm) & 0xFFFFFFFF) - BASES[pc]
            out[pc] = (w & 0xFFFF0000) | (n & 0xFFFF)
            notes.append(f"{pc:08X}: through the game's register, EU offset {n:#x}")
            continue
        if w in us_hook_words and us_hook_words[w] != w:
            out[pc] = us_hook_words[w]
            notes.append(f"{pc:08X}: the hooked instruction, EU form")
            continue
        if op in (2, 3):
            t = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if t:
                n = reloc(t)
                out[pc] = (w & 0xFC000000) | ((n >> 2) & 0x03FFFFFF)
            continue
        if op == 0x0F:
            rt = (w >> 16) & 31
            regs[rt] = (pc, (w & 0xFFFF) << 16)
            pending.setdefault(pc, [])
            continue
        if op in LO_OPS and (w >> 21) & 31 in regs:
            lp, h = regs[(w >> 21) & 31]
            imm = w & 0xFFFF
            full = (h | imm) if op == 0x0D else (h + sext(imm)) & 0xFFFFFFFF
            pending[lp].append((pc, full, op == 0x0D))
        # a write to a register ends what it held
        if op == 0:
            rd = (w >> 11) & 31
            regs.pop(rd, None)
        elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x20, 0x21, 0x23, 0x24, 0x25):
            regs.pop((w >> 16) & 31, None)
        if op in (2, 3, 4, 5, 6, 7) or (op == 0 and w & 0x3F in (8, 9)):
            regs = {}
    for lp, uses in pending.items():
        if not uses:
            continue
        his = set()
        for up, full, ori in uses:
            if 0x08000000 <= full < 0x0A000000 or 0x04000000 <= full < 0x04800000:
                n = reloc(full)
            elif full >> 26 == 2 and own(((up + 4) & 0xF0000000) | ((full & 0x03FFFFFF) << 2)):
                # a `j` into the mod, built to be written into the game's code
                n = (full & 0xFC000000) | ((reloc(((up + 4) & 0xF0000000) | ((full & 0x03FFFFFF) << 2)) >> 2) & 0x03FFFFFF)
            else:
                n = full                                  # not an address (a constant, a float)
            lo16 = n & 0xFFFF
            hi16 = (n >> 16) if ori else ((n - sext(lo16)) >> 16) & 0xFFFF
            his.add(hi16)
            out[up] = (out[up] & 0xFFFF0000) | lo16
        if len(his) != 1:
            raise ValueError(f"{mid}: lui at {lp:08X} serves addresses that moved apart: {[hex(h) for h in his]}")
        out[lp] = (out[lp] & 0xFFFF0000) | his.pop()
    # data: GE lists (BASE + VADDR/IADDR into the mod; texture addresses into the game)
    base_hi = 0
    data_words = sorted(a for a in out if a not in code)
    for a in data_words:
        w = out[a]
        cmd = w >> 24
        if cmd == 0x10:
            base_hi = (w >> 16) & 0xFF
            nxt = [out.get(a + 4 * k, 0) for k in range(1, 4)]
            if any(x >> 24 in (1, 2) and own((base_hi << 24) | (x & 0xFFFFFF)) for x in nxt):
                out[a] = (w & 0xFF00FFFF) | ((((base_hi << 24) + delta) >> 24) << 16)
        elif cmd in (1, 2) and own((base_hi << 24) | (w & 0xFFFFFF)):
            n = ((base_hi << 24) | (w & 0xFFFFFF)) + delta
            out[a] = (cmd << 24) | (n & 0xFFFFFF)
        elif 0xA0 <= cmd <= 0xA7 and out.get(a + 0x20, 0) >> 24 == cmd + 8:
            pass                                          # TBP with its TBW 8 words on: handled below
        elif own(w):
            out[a] = w + delta
            notes.append(f"{a:08X}: a pointer into the mod")
    for a in data_words:
        w = out[a]
        cmd = w >> 24
        if 0xA0 <= cmd <= 0xA7:
            for k in range(1, 12):                         # its TBW (address bits 24-31)
                b = out.get(a + 4 * k, 0)
                if b >> 24 == cmd + 8:
                    t = ((b >> 16) & 0xFF) << 24 | (w & 0xFFFFFF)
                    n = reloc(t)
                    out[a] = (cmd << 24) | (n & 0xFFFFFF)
                    out[a + 4 * k] = (b & 0xFF00FFFF) | ((n >> 24) << 16)
                    break

    # ---- the hooks, in EU terms ----
    def hook_entry(a, w, unit):
        e = us2eu(a)
        orig = struct.unpack_from("<I", EU[1], e - EU[0])[0] if unit == "eboot" else None
        if w >> 26 == 2:
            t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            n = reloc(t)
            w2 = (w & 0xFC000000) | ((n >> 2) & 0x03FFFFFF)
            return {"addr": e, "unit": unit, "orig": orig, "word": w2, "kind": "j", "target": n}
        return {"addr": e, "unit": unit, "orig": orig, "word": w, "kind": "word"}

    hk = [hook_entry(a, w, "eboot") for a, w in hooks]
    # what the mod writes into overlays at run time: lui/addiu t0 = address, lui/addiu t1 = word, sw t1,(t0)
    for a in range(lo, hi, 4):
        if a in code and out[a] >> 16 == 0xAD09 and (out[a] & 0xFFFF) == 0:
            # the four instructions before build t0 and t1 (already rewritten)
            t0 = (((out[a - 16] & 0xFFFF) << 16) + sext(out[a - 12] & 0xFFFF)) & 0xFFFFFFFF
            t1 = (((out[a - 8] & 0xFFFF) << 16) + sext(out[a - 4] & 0xFFFF)) & 0xFFFFFFFF
            if 0x09A5F000 <= t0 < 0x09D63000:
                unit = "game_task" if t0 < 0x09C19000 else "game_sub"
                kind = "j" if t1 >> 26 == 2 else "word"
                e = {"addr": t0, "unit": unit, "orig": None, "word": t1, "kind": kind}
                if kind == "j":
                    e["target"] = ((t0 + 4) & 0xF0000000) | ((t1 & 0x03FFFFFF) << 2)
                hk.append(e)
    # where the mods jump back into the game: entries the recompiled game needs
    backs = set()
    for a in code:
        w = out.get(a, 0)
        if w >> 26 == 2:
            t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if not (lo + delta <= t < hi + delta):
                backs.add(t)
    words = [(a + delta, out[a]) for a in range(lo, hi, 4)]
    return {
        "id": mid, "name": name, "description": desc, "default": True,
        "base": lo + delta, "size": hi - lo,
        "entries": sorted(e + delta for e in entries),
        "hooks": hk, "returns": sorted(backs),
        "notes": notes,
    }, words


def overlay_word(unit, a):
    """The EU word at `a` in the named overlay (work/overlays, as the host saved it)."""
    for f in sorted(glob.glob(os.path.join(REPO, "work", "overlays", "*.bin"))):
        d = open(f, "rb").read()
        if d[:4] != b"MWo3" or not d[0x20:0x40].startswith(unit.encode() + b".ovl"):
            continue
        base, code = struct.unpack_from("<II", d, 8)
        if base <= a < base + 0x40 + code:            # the header loads too: code at base + 0x40
            return struct.unpack_from("<I", d, a - base)[0]
    raise ValueError(f"{a:08X}: no {unit} overlay in work/overlays")


def mnemonic(w, a):
    from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    i = next(md.disasm(struct.pack("<I", w), a), None)
    return i.mnemonic if i else "?"


def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def add_seeds(path, header, addrs, why):
    """Addresses into a seeds file, once each."""
    have = set()
    if os.path.exists(path):
        for l in open(path):
            l = l.split("#")[0].strip()
            if l:
                have.add(int(l, 16))
    new = [a for a in sorted(set(addrs)) if a not in have]
    if not new:
        return
    fresh = not os.path.exists(path)
    with open(path, "a", newline="\n") as f:
        if fresh:
            f.write(header)
        f.write(f"# {why}\n")
        for a in new:
            f.write(f"{a:08X}\n")


def write_ovl(path, mid, base, words):
    """An "MWo3" overlay: 0x40-byte header, then the code (and data, as code).
    The header loads too, so its load address is 0x40 before the code."""
    body = b"".join(struct.pack("<I", w) for _, w in words)
    name = f"mhfu_mod_{mid}.ovl".encode()
    hdr = struct.pack("<4sIIIIIII", b"MWo3", 0x4D4F44, base - 0x40, len(body), 0, 0, 0, 0) + name.ljust(32, b"\0")[:32]
    open(path, "wb").write(hdr + body)
    return fnv1a(hdr + body)


def main():
    global EU, US
    src = sys.argv[1] if len(sys.argv) > 1 else r"E:\Emulator Games\PSP"
    eu = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "work", "ULES01213_EBOOT.BIN")
    us = sys.argv[3] if len(sys.argv) > 3 else os.path.join(src, "ULUS10391_EBOOT.BIN")
    EU, US = load_elf(eu), load_elf(us)
    outdir = os.path.join(REPO, "mods", "builtin")
    os.makedirs(outdir, exist_ok=True)
    meta = []
    for prefix, mid, name, desc, delta in MODS:
        folder = [d for d in glob.glob(os.path.join(src, prefix + "*")) if os.path.isdir(d)]
        if not folder:
            print(f"mods: no folder for {name} under {src}")
            continue
        m, words = convert(folder[0], mid, name, desc, delta)
        h = write_ovl(os.path.join(outdir, mid + ".bin"), mid, m["base"], words)
        m["overlay"] = f"{h:08X}"
        # the original EU words at the hooks (to take a mod out again), and what they are
        for k in m["hooks"]:
            if k["unit"] != "eboot":
                k["orig"] = overlay_word(k["unit"], k["addr"])
            k["mnemonic"] = mnemonic(k["orig"], k["addr"])
        # a hook that also replaced the delay slot (with a nop) belongs to the `j` before it
        addrs = {k["addr"]: k for k in m["hooks"]}
        for k in m["hooks"]:
            if k["kind"] == "word" and k["unit"] == "eboot" and addrs.get(k["addr"] - 4, {}).get("kind") == "j":
                addrs[k["addr"] - 4]["delay"] = k["word"]
                k["companion"] = True
        # seeds: the mod's entry points, and where it jumps back into the game
        add_seeds(os.path.join(REPO, "seeds", f"ov{h:08X}.txt"),
                  f"# {name} (built-in mod, tools/mods_convert.py): where the game's hooks jump.\n",
                  m["entries"], "hook targets")
        add_seeds(os.path.join(REPO, "seeds", "eboot.txt"), "", [r for r in m["returns"] if r < 0x08A00000],
                  f"where the built-in mod {name} jumps back (tools/mods_convert.py)")
        add_seeds(os.path.join(REPO, "seeds", "ovC5DF3522.txt"), "",
                  [r for r in m["returns"] if 0x09A5F200 <= r < 0x09C19000],
                  f"where the built-in mod {name} jumps back (tools/mods_convert.py)")
        meta.append(m)
        print(f"mods: {name}: {len(words)} words at {m['base']:08X}, {len(m['hooks'])} hook(s), "
              f"{len(m['returns'])} return(s)")
        for h in m["hooks"]:
            print(f"   hook {h['unit']:9} {h['addr']:08X} {h['kind']:4} {h['word']:08X}" + (f" -> {h['target']:08X}" if 'target' in h else ""))
        for r in m["returns"]:
            print(f"   back to {r:08X}")
        for n in m["notes"]:
            print("   " + n)
    json.dump(meta, open(os.path.join(outdir, "mods.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
