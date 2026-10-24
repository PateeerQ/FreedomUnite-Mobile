"""Tiny MIPS (Allegrex) disassembler for the decrypted EBOOT.

    pip install capstone
    python tools/re/mdis.py 088BF858 40        # disassemble 40 instructions
    python tools/re/mdis.py xref 088BF858      # every jal to that address

The EBOOT defaults to work/ULES01213_EBOOT.BIN; set MHFU_EBOOT to use another.
VFPU instructions show as .word (capstone does not know them).
"""
import os, struct, sys
import capstone

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EB = os.environ.get('MHFU_EBOOT', os.path.join(REPO, 'work', 'ULES01213_EBOOT.BIN'))
d = open(EB, 'rb').read()
phoff = struct.unpack_from('<I', d, 28)[0]
phnum = struct.unpack_from('<H', d, 44)[0]
segs = []
for i in range(phnum):
    t, off, va, pa, fs, ms = struct.unpack_from('<6I', d, phoff + 32 * i)
    if t == 1:
        segs.append((va, off, fs))

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)


def rd(a, n):
    for va, off, fs in segs:
        if va <= a < va + fs:
            return d[off + a - va:off + a - va + n]
    return None


def dis(a, n):
    b = rd(a, n * 4)
    for i in range(len(b) // 4):
        w = struct.unpack_from('<I', b, i * 4)[0]
        ins = list(md.disasm(b[i * 4:i * 4 + 4], a + i * 4))
        s = f'{ins[0].mnemonic} {ins[0].op_str}' if ins else f'.word {w:08x}'
        print(f'{a + i * 4:08x}: {w:08x}  {s}')


def xref(t):
    w = 0x0C000000 | ((t >> 2) & 0x3FFFFFF)
    return [va + i for va, off, fs in segs for i in range(0, fs, 4)
            if struct.unpack_from('<I', d, off + i)[0] == w]


if __name__ == '__main__':
    if sys.argv[1] == 'xref':
        print(' '.join(f'{x:08X}' for x in xref(int(sys.argv[2], 16))))
    else:
        dis(int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 32)
