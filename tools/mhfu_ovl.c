/* mhfu_ovl -- turn an overlay saved by mhfu_host into an ELF allegrexrecomp
 * can recompile.
 *
 *   mhfu_ovl <slotN_xxxx.bin> <out.elf> [main-seeds.txt]
 *
 * prints one line, "HASH BASE NAME", that setup.ps1 reads. With a third
 * argument, the targets of every jal/j in the overlay's code that lands
 * outside it are appended there, one per line: functions of the EBOOT that
 * only overlays call, which discovery over the EBOOT alone cannot find
 * (allegrexrecomp emit --seeds).
 *
 * An MHFU overlay ("MWo3") is a 0x40-byte header, then code, then data:
 *
 *   +0  magic 'MWo3'       +12 code size       +24 constructor table start
 *   +4  id                 +16 data size       +28 constructor table end
 *   +8  load address       +20 bss size        +32 name, e.g. "demo_sub.ovl"
 *
 * It has no symbols and no relocations, so the seeds discovery needs are found
 * by shape and handed over as synthetic relocations: R_MIPS_32 on every word
 * holding an address inside the overlay, HI16/LO16 on every lui/addiu pair
 * that builds one, and R_MIPS_32 on a table of likely function starts (the
 * first word after a `jr $ra` or `j` and its delay slot, padding allowed, or
 * after two or more words of zero padding) appended past the overlay's end.
 *
 * The hash is FNV-1a over header + code (not the data, which the game writes
 * into), the same bytes and function hle_mhfu.c uses to recognise the overlay when the game loads it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVL_MAGIC 0x336F574Du
#define HDR       0x40u

enum { R_MIPS_32 = 2, R_MIPS_HI16 = 5, R_MIPS_LO16 = 6 };

static uint8_t *g_d;
static uint32_t g_n, g_base;

static uint32_t rd(uint32_t o) {
    return g_d[o] | (g_d[o + 1] << 8) | (g_d[o + 2] << 16) | ((uint32_t)g_d[o + 3] << 24);
}

static void wr(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

typedef struct { uint32_t off, info; } rel;
static rel     *g_rel;
static uint32_t g_nrel, g_caprel;

static void add_rel(uint32_t off, uint32_t type) {
    if (g_nrel == g_caprel) {
        g_caprel = g_caprel ? g_caprel * 2 : 256;
        g_rel = (rel *)realloc(g_rel, g_caprel * sizeof *g_rel);
        if (!g_rel) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    g_rel[g_nrel].off = off;
    g_rel[g_nrel].info = type;
    g_nrel++;
}

uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: mhfu_ovl <overlay.bin> <out.elf> [main-seeds.txt]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < (long)HDR) { fprintf(stderr, "%s: too small\n", argv[1]); return 1; }
    uint8_t *file = (uint8_t *)malloc((size_t)len);
    if (!file || fread(file, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    g_d = file;

    if (rd(0) != OVL_MAGIC) { fprintf(stderr, "%s: not an MWo3 overlay\n", argv[1]); return 1; }
    g_base = rd(8);
    uint32_t code = rd(12), data = rd(16);
    uint32_t end = HDR + code + data;
    if (end > (uint32_t)len || code > (uint32_t)len) {
        fprintf(stderr, "%s: header sizes run past the file\n", argv[1]);
        return 1;
    }
    g_n = end;
    uint32_t hash = fnv1a(file, HDR + code);
    char name[33] = {0};
    memcpy(name, file + 32, 32);
    for (int i = 0; i < 32 && name[i]; i++) if (name[i] < 33 || name[i] > 126) name[i] = '_';

    const uint32_t lo = g_base + HDR, hi = g_base + end;
    uint32_t ct = rd(24), ct_end = rd(28);
    uint32_t entry = g_base + HDR;
    if (ct_end > ct && ct >= g_base && ct + 4 <= hi) entry = rd(ct - g_base);

    /* Stored pointers. */
    for (uint32_t o = HDR; o + 4 <= end; o += 4) {
        uint32_t v = rd(o);
        if (v >= lo && v < hi && !(v & 3)) add_rel(g_base + o, R_MIPS_32);
    }
    /* Built pointers: lui rt, hi ; ... ; addiu rx, rt, lo */
    for (uint32_t o = HDR; o + 4 <= end; o += 4) {
        uint32_t ins = rd(o);
        if (ins >> 26 != 0x0F) continue;
        uint32_t rt = (ins >> 16) & 31, h16 = ins & 0xFFFF;
        for (uint32_t k = 1; k <= 8 && o + 4 * k + 4 <= end; k++) {
            uint32_t p = o + 4 * k, j = rd(p), op = j >> 26;
            if (op == 0x09 && ((j >> 21) & 31) == rt) {
                uint32_t t = (h16 << 16) + (uint32_t)(int32_t)(int16_t)(j & 0xFFFF);
                if (t >= lo && t < hi && !(t & 3)) {
                    add_rel(g_base + o, R_MIPS_HI16);
                    add_rel(g_base + p, R_MIPS_LO16);
                }
                break;
            }
            if (((j >> 16) & 31) == rt && op != 0x2B && op != 0x29 && op != 0x28) break;
        }
    }
    /* Likely function starts, as a pointer table past the end. */
    uint32_t seg_len = (end + 15) & ~15u;
    uint32_t nstart = 0;
    uint32_t *starts = (uint32_t *)malloc((code / 4 + 1) * sizeof *starts);
    if (!starts) return 1;
    for (uint32_t o = HDR + 8; o < HDR + code; o += 4) {
        if (!rd(o)) continue;
        /* Step back over alignment padding; `p` is then the last word of
         * whatever precedes it. */
        uint32_t p = o - 4, zeros = 0;
        while (p > HDR && !rd(p)) { p -= 4; zeros++; }
        /* The delay slot may be a nop, which the loop above took for padding:
         * then `p` is the jump itself. Missing that case left 40 functions of
         * game_sub.ovl undiscovered (each a dispatch miss in the village). */
        /* `jr $t9` is a tail call through a pointer: game_task.ovl's virtual
         * call thunks end in it, and the function after one was only ever
         * reached through a pointer (a dispatch miss on the way to a quest).
         * Other `jr` registers are left out: those are switch tables. */
        int after_return = p >= HDR + 4 && (rd(p - 4) == 0x03E00008u ||  /* jr $ra; delay */
                                            rd(p - 4) == 0x03200008u ||   /* jr $t9; delay */
                                            (rd(p - 4) >> 26) == 0x02 ||  /* j; delay      */
                                            (zeros && (rd(p) == 0x03E00008u || rd(p) == 0x03200008u ||
                                                       (rd(p) >> 26) == 0x02)));
        if (after_return || zeros >= 2) starts[nstart++] = g_base + o;
    }
    for (uint32_t i = 0; i < nstart; i++) add_rel(g_base + seg_len + 4 * i, R_MIPS_32);
    uint32_t seg_total = seg_len + 4 * nstart;

    /* ELF: header, one PT_LOAD (r-x), sections .text (code only), .shstrtab, .rel.text */
    static const char shstr[] = "\0.text\0.shstrtab\0.rel.text";
    const uint32_t shstr_len = sizeof shstr;
    const uint32_t segoff = 0x100, reloff = segoff + seg_total;
    const uint32_t shstroff = reloff + 8 * g_nrel;
    const uint32_t shoff = (shstroff + shstr_len + 3) & ~3u;
    const uint32_t total = shoff + 4 * 40;
    uint8_t *out = (uint8_t *)calloc(1, total);
    if (!out) return 1;

    memcpy(out, "\x7f" "ELF\x01\x01\x01", 7);
    wr16(out + 16, 2);  wr16(out + 18, 8);  wr(out + 20, 1);
    wr(out + 24, entry); wr(out + 28, 52); wr(out + 32, shoff); wr(out + 36, 0);
    wr16(out + 40, 52); wr16(out + 42, 32); wr16(out + 44, 1);
    wr16(out + 46, 40); wr16(out + 48, 4);  wr16(out + 50, 2);

    uint8_t *ph = out + 52;
    wr(ph + 0, 1); wr(ph + 4, segoff); wr(ph + 8, g_base); wr(ph + 12, g_base);
    wr(ph + 16, seg_total); wr(ph + 20, seg_total); wr(ph + 24, 5); wr(ph + 28, 16);

    memcpy(out + segoff, file, end);
    for (uint32_t i = 0; i < nstart; i++) wr(out + segoff + seg_len + 4 * i, starts[i]);
    for (uint32_t i = 0; i < g_nrel; i++) {
        wr(out + reloff + 8 * i, g_rel[i].off);
        wr(out + reloff + 8 * i + 4, g_rel[i].info);
    }
    memcpy(out + shstroff, shstr, shstr_len);

    uint8_t *sh = out + shoff + 40;                     /* [0] is the null section */
    wr(sh + 0, 1); wr(sh + 4, 1); wr(sh + 8, 6); wr(sh + 12, g_base + HDR);
    wr(sh + 16, segoff + HDR); wr(sh + 20, code); wr(sh + 32, 4);
    sh += 40;
    wr(sh + 0, 7); wr(sh + 4, 3); wr(sh + 16, shstroff); wr(sh + 20, shstr_len); wr(sh + 32, 1);
    sh += 40;
    wr(sh + 0, 17); wr(sh + 4, 9); wr(sh + 16, reloff); wr(sh + 20, 8 * g_nrel);
    wr(sh + 28, 1); wr(sh + 32, 4); wr(sh + 36, 8);

    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, total, f) != total) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fclose(f);

    if (argc == 4) {
        FILE *sf = fopen(argv[3], "a");
        if (!sf) { fprintf(stderr, "cannot append to %s\n", argv[3]); return 1; }
        fprintf(sf, "# called from %s (%08X)\n", name, hash);
        for (uint32_t o = HDR; o + 4 <= HDR + code; o += 4) {
            uint32_t ins = rd(o), op = ins >> 26;
            if (op != 0x02 && op != 0x03) continue;                  /* j, jal */
            uint32_t t = ((g_base + o + 4) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2);
            if (t >= g_base && t < g_base + end) continue;          /* its own code */
            if (t < 0x08804000u || t >= 0x0A000000u) continue;      /* not user memory */
            fprintf(sf, "%08X\n", t);
        }
        fclose(sf);
    }

    printf("%08X %08X %s\n", hash, g_base, name[0] ? name : "(unnamed)");
    fprintf(stderr, "%s: %s at 0x%08X, %u bytes, %u seeds\n", argv[1], name, g_base, end, g_nrel);
    return 0;
}
