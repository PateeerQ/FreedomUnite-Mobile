/* MHFU-Android -- sceLibFont: the PSP's system font library.
 *
 * Games draw text with the firmware's PGF fonts (the .pgf files in flash0:/font): they ask
 * for a font by style, then for each character's metrics and a 4- or 8-bit
 * coverage bitmap, which they upload as a texture. MHFU uses it for every
 * line of text after the language screen.
 *
 * The fonts are not part of the game, so the host reads them from
 * <root>/flash0/font/ (setup.ps1 copies them there from PPSSPP's install, or
 * from a folder the user names). Only the calls MHFU imports are here.
 *
 * Behaviour follows PPSSPP's sceFont/PGF implementation, which was measured
 * against hardware: glyph drawing *adds* coverage with saturation, a
 * fractional x position blends with the left neighbour, only the 4- and 8-bit
 * formats are drawn, and a character missing from the font falls back to
 * '_' unless it is below the font's first glyph. The game's alloc/free
 * callbacks are not called; nothing here needs guest memory of its own. */
#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfont.h"

#define SCE_FONT_ERROR_INVALID_LIBID     0x80460002u
#define SCE_FONT_ERROR_INVALID_PARAMETER 0x80460003u
#define SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS 0x80460009u

#define PGF_HEADER_SIZE  392
#define PGF_REV3_EXTRA   16
#define MAX_OPEN         16
#define LIB_HANDLE       0x0F000001u
#define FONT_HANDLE_BASE 0x0F010000u
#define ALT_CHAR         0x5F

enum { FLAG_BMP_OVERLAY = 0x03, FLAG_H_ROWS = 0x01, FLAG_V_ROWS = 0x02,
       FLAG_DIM = 0x04, FLAG_XADJ = 0x08, FLAG_YADJ = 0x10, FLAG_ADV = 0x20 };

enum { FAMILY_SANS = 1, FAMILY_SERIF = 2 };
enum { STYLE_REGULAR = 1, STYLE_ITALIC = 2, STYLE_BOLD = 5, STYLE_BOLD_ITALIC = 6, STYLE_DB = 103 };
enum { LANG_JAPANESE = 1, LANG_LATIN = 2, LANG_KOREAN = 3 };

/* The fonts a PSP has, in its registry order (index = what FindOptimumFont
 * returns and Open takes). Sizes are 26.6 fixed point. */
typedef struct {
    int hsize, vsize, hres, vres, family, style, language, country;
    const char *file, *name;
} font_entry;

static const font_entry k_registry[] = {
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SANS,  STYLE_DB,          LANG_JAPANESE, 1, "jpn0.pgf",  "FTT-NewRodin Pro DB" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SANS,  STYLE_REGULAR,     LANG_LATIN,    1, "ltn0.pgf",  "FTT-NewRodin Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SERIF, STYLE_REGULAR,     LANG_LATIN,    1, "ltn1.pgf",  "FTT-Matisse Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SANS,  STYLE_ITALIC,      LANG_LATIN,    1, "ltn2.pgf",  "FTT-NewRodin Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SERIF, STYLE_ITALIC,      LANG_LATIN,    1, "ltn3.pgf",  "FTT-Matisse Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SANS,  STYLE_BOLD,        LANG_LATIN,    1, "ltn4.pgf",  "FTT-NewRodin Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SERIF, STYLE_BOLD,        LANG_LATIN,    1, "ltn5.pgf",  "FTT-Matisse Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SANS,  STYLE_BOLD_ITALIC, LANG_LATIN,    1, "ltn6.pgf",  "FTT-NewRodin Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN,    1, "ltn7.pgf",  "FTT-Matisse Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SANS,  STYLE_REGULAR,     LANG_LATIN,    1, "ltn8.pgf",  "FTT-NewRodin Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SERIF, STYLE_REGULAR,     LANG_LATIN,    1, "ltn9.pgf",  "FTT-Matisse Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SANS,  STYLE_ITALIC,      LANG_LATIN,    1, "ltn10.pgf", "FTT-NewRodin Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SERIF, STYLE_ITALIC,      LANG_LATIN,    1, "ltn11.pgf", "FTT-Matisse Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SANS,  STYLE_BOLD,        LANG_LATIN,    1, "ltn12.pgf", "FTT-NewRodin Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SERIF, STYLE_BOLD,        LANG_LATIN,    1, "ltn13.pgf", "FTT-Matisse Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SANS,  STYLE_BOLD_ITALIC, LANG_LATIN,    1, "ltn14.pgf", "FTT-NewRodin Pro Latin" },
    { 0x1c0, 0x1c0, 0x2000, 0x2000, FAMILY_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN,    1, "ltn15.pgf", "FTT-Matisse Pro Latin" },
    { 0x288, 0x288, 0x2000, 0x2000, FAMILY_SANS,  STYLE_REGULAR,     LANG_KOREAN,   3, "kr0.pgf",   "AsiaNHH(512Johab)" },
};
#define NREG ((int)(sizeof k_registry / sizeof k_registry[0]))

/* ---- PGF ------------------------------------------------------------------- */

typedef struct {
    int w, h, left, top, flags;
    int shadow_flags, shadow_id;
    int adv_h, adv_v, dim_w, dim_h, xadj_h, xadj_v, yadj_h, yadj_v;
    uint32_t ptr;          /* bit offset of the bitmap in data */
} glyph;

typedef struct {
    const font_entry *entry;
    uint8_t  *data;        /* glyph data, padded so bit reads can overrun */
    size_t    size;
    int       first_glyph, nglyphs, ncharmap;
    int      *charmap;
    glyph    *glyphs;
    int       bpp;
    int32_t   max_ascender, max_descender, max_leftx, max_basey, min_centerx, max_topy;
    int32_t   max_adv[2], max_size[2];
    uint16_t  max_glyph_w, max_glyph_h;
} pgf;

static uint32_t bits(const uint8_t *buf, size_t pos, int n) {
    uint64_t v = 0;
    size_t byte = pos >> 3;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[byte + (size_t)i] << (8 * i);
    v >>= pos & 7;
    return n >= 32 ? (uint32_t)v : (uint32_t)(v & ((1ull << n) - 1));
}

static uint32_t take(const uint8_t *buf, size_t *pos, int n) {
    uint32_t v = bits(buf, *pos, n);
    *pos += (size_t)n;
    return v;
}

static int32_t rd32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int read_glyph(const pgf *f, const int32_t *tables[4][2], const int tlen[4],
                      size_t pos, glyph *g) {
    if (pos + 1024 > f->size * 8) return -1;
    pos += 14;                                   /* glyph size */
    g->w = (int)take(f->data, &pos, 7);
    g->h = (int)take(f->data, &pos, 7);
    g->left = (int)take(f->data, &pos, 7); if (g->left >= 64) g->left -= 128;
    g->top  = (int)take(f->data, &pos, 7); if (g->top  >= 64) g->top  -= 128;
    g->flags = (int)take(f->data, &pos, 6);
    g->shadow_flags  = (int)take(f->data, &pos, 2) << 5;
    g->shadow_flags |= (int)take(f->data, &pos, 2) << 3;
    g->shadow_flags |= (int)take(f->data, &pos, 3);
    g->shadow_id = (int)take(f->data, &pos, 9);

    int *out[4][2] = { { &g->dim_w, &g->dim_h }, { &g->xadj_h, &g->xadj_v },
                       { &g->yadj_h, &g->yadj_v }, { &g->adv_h, &g->adv_v } };
    static const int k_flag[4] = { FLAG_DIM, FLAG_XADJ, FLAG_YADJ, FLAG_ADV };
    for (int t = 0; t < 4; t++) {
        if (g->flags & k_flag[t]) {
            int i = (int)take(f->data, &pos, 8);
            if (i < tlen[t]) { *out[t][0] = tables[t][0][i]; *out[t][1] = tables[t][1][i]; }
        } else {
            *out[t][0] = (int32_t)take(f->data, &pos, 32);
            *out[t][1] = (int32_t)take(f->data, &pos, 32);
        }
    }
    g->ptr = (uint32_t)pos;
    return 0;
}

static int pgf_load(pgf *f, const uint8_t *d, size_t n) {
    if (n < PGF_HEADER_SIZE || memcmp(d + 4, "PGF0", 4) != 0) return -1;
    int rev = rd32(d + 8);
    int charmap_len = rd32(d + 16), charptr_len = rd32(d + 20);
    int charmap_bpe = rd32(d + 24), charptr_bpe = rd32(d + 28);
    f->bpp = d[34];
    f->first_glyph = rd16(d + 182);
    f->max_ascender = rd32(d + 212);  f->max_descender = rd32(d + 216);
    f->max_leftx = rd32(d + 220);     f->max_basey = rd32(d + 224);
    f->min_centerx = rd32(d + 228);   f->max_topy = rd32(d + 232);
    f->max_adv[0] = rd32(d + 236);    f->max_adv[1] = rd32(d + 240);
    f->max_size[0] = rd32(d + 244);   f->max_size[1] = rd32(d + 248);
    f->max_glyph_w = rd16(d + 252);   f->max_glyph_h = rd16(d + 254);
    int tlen[4] = { d[258], d[259], d[260], d[261] };
    int shadow_len = rd32(d + 364), shadow_bpe = rd32(d + 368);

    if (charmap_len < 0 || charmap_len > 0x100000 || charptr_len < 0 || charptr_len > 0x100000 ||
        shadow_len < 0 || shadow_len > 0x10000 ||
        charmap_bpe <= 0 || charmap_bpe > 32 || charptr_bpe <= 0 || charptr_bpe > 32 ||
        shadow_bpe < 0 || shadow_bpe > 32)
        return -1;

    size_t at = PGF_HEADER_SIZE;
    int comp1 = 0, comp2 = 0;
    if (rev == 3) {
        if (n < at + PGF_REV3_EXTRA) return -1;
        comp1 = rd32(d + at + 4) & 0xFFFF;
        comp2 = rd32(d + at + 12) & 0xFFFF;
        at += PGF_REV3_EXTRA;
    }
    size_t tables = (size_t)(tlen[0] + tlen[1] + tlen[2] + tlen[3]) * 8;
    size_t shadow_sz = (((size_t)shadow_len * (size_t)shadow_bpe + 31) & ~(size_t)31) / 8;
    size_t comp_sz = (size_t)(comp1 + comp2) * 4;
    size_t map_sz = (((size_t)charmap_len * (size_t)charmap_bpe + 31) & ~(size_t)31) / 8;
    size_t ptr_sz = (((size_t)charptr_len * (size_t)charptr_bpe + 31) & ~(size_t)31) / 8;
    if (at + tables + shadow_sz + comp_sz + map_sz + ptr_sz >= n) return -1;

    int32_t *tab = (int32_t *)malloc(tables ? tables : 1);
    if (!tab) return -1;
    const int32_t *tp[4][2];
    for (int t = 0, k = 0; t < 4; t++) {
        tp[t][0] = tab + k;
        tp[t][1] = tab + k + tlen[t];
        for (int i = 0; i < tlen[t]; i++) {
            tab[k + i]           = rd32(d + at);
            tab[k + tlen[t] + i] = rd32(d + at + 4);
            at += 8;
        }
        k += 2 * tlen[t];
    }
    at += shadow_sz + comp_sz;
    const uint8_t *map = d + at;      at += map_sz;
    at += ptr_sz;                     /* char pointers follow the map */

    f->size = n - at;
    f->data = (uint8_t *)calloc(1, f->size + 256);
    f->charmap = (int *)malloc(sizeof(int) * (size_t)(charmap_len ? charmap_len : 1));
    f->glyphs = (glyph *)calloc((size_t)(charptr_len ? charptr_len : 1), sizeof(glyph));
    /* Bit tables are read through a padded copy, as reads go up to 8 bytes on. */
    uint8_t *mp = (uint8_t *)calloc(1, map_sz + ptr_sz + 16);
    if (!f->data || !f->charmap || !f->glyphs || !mp) { free(tab); free(mp); return -1; }
    memcpy(f->data, d + at, f->size);
    memcpy(mp, map, map_sz + ptr_sz);
    f->ncharmap = charmap_len;
    f->nglyphs = charptr_len;

    for (int i = 0; i < charmap_len; i++) {
        int v = (int)bits(mp, (size_t)i * (size_t)charmap_bpe, charmap_bpe);
        f->charmap[i] = v < charptr_len ? v : 65535;
    }
    for (int i = 0; i < charptr_len; i++) {
        size_t p = (size_t)bits(mp + map_sz, (size_t)i * (size_t)charptr_bpe, charptr_bpe) * 32;
        read_glyph(f, tp, tlen, p, &f->glyphs[i]);
    }
    free(mp);
    free(tab);
    return 0;
}

static const glyph *pgf_glyph(const pgf *f, int code) {
    if (code < f->first_glyph) return NULL;
    code -= f->first_glyph;
    if (code < f->ncharmap) code = f->charmap[code];
    return code < f->nglyphs ? &f->glyphs[code] : NULL;
}

/* The glyph for a character, or for '_' when it has none -- except below the
 * first glyph, which draws nothing. */
static const glyph *find_glyph(const pgf *f, int code) {
    const glyph *g = pgf_glyph(f, code);
    if (!g && code >= f->first_glyph) g = pgf_glyph(f, ALT_CHAR);
    return g;
}

/* ---- the library ------------------------------------------------------------- */

static char     g_dir[600];
static pgf      g_font[NREG];
static int      g_loaded[NREG], g_tried;
static int      g_open[MAX_OPEN];            /* registry index + 1, 0 = free */
static uint64_t g_glyphs_drawn;

uint64_t mhfu_libfont_glyphs(void) { return g_glyphs_drawn; }

static void load_fonts(void) {
    if (g_tried) return;
    g_tried = 1;
    int n = 0;
    for (int i = 0; i < NREG; i++) {
        char path[700];
        snprintf(path, sizeof path, "%s/%s", g_dir, k_registry[i].file);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint8_t *buf = len > 0 ? (uint8_t *)malloc((size_t)len) : NULL;
        if (buf && fread(buf, 1, (size_t)len, fp) == (size_t)len && pgf_load(&g_font[i], buf, (size_t)len) == 0) {
            g_font[i].entry = &k_registry[i];
            g_loaded[i] = 1;
            n++;
        } else {
            fprintf(stderr, "mhfu: font %s is not a PGF font\n", path);
        }
        free(buf);
        fclose(fp);
    }
    if (n) printf("libfont: %d font(s) from %s\n", n, g_dir);
    else fprintf(stderr, "mhfu: no PSP fonts in %s -- text will be blank (see README, Fonts)\n", g_dir);
}

static void set_error(uint32_t at, uint32_t code) { if (at) psp_write32(at, code); }

static const pgf *open_font(uint32_t handle) {
    uint32_t i = handle - FONT_HANDLE_BASE;
    if (i >= MAX_OPEN || !g_open[i]) return NULL;
    return &g_font[g_open[i] - 1];
}

/* (SceFontNewLibParams *params, u32 *error) -> library handle */
static void hle_NewLib(void) {
    load_fonts();
    set_error(psp_arg(1), 0);
    psp_ret(LIB_HANDLE);
}

static void hle_DoneLib(void) {
    memset(g_open, 0, sizeof g_open);
    psp_ret(0);
}

/* (lib, u32 *error) */
static void hle_GetNumFontList(void) {
    if (psp_arg(0) != LIB_HANDLE) { set_error(psp_arg(1), SCE_FONT_ERROR_INVALID_LIBID); psp_ret(0); return; }
    set_error(psp_arg(1), 0);
    int n = 0;
    for (int i = 0; i < NREG; i++) n += g_loaded[i];
    psp_ret((uint32_t)n);
}

static float rdf(uint32_t at) {
    uint32_t u = psp_read32(at);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* SceFontStyle: float h, v, hres, vres, weight; u16 family, style, stylesub,
 * language, region, country; char name[64], file[64]; u32 attributes, expire. */
static int style_matches(uint32_t st, const font_entry *e) {
    const uint16_t fam = (uint16_t)psp_read16(st + 20), sty = (uint16_t)psp_read16(st + 22);
    const uint16_t lang = (uint16_t)psp_read16(st + 26), country = (uint16_t)psp_read16(st + 30);
    if (fam && fam != e->family) return 0;
    if (sty && sty != e->style) return 0;
    if (lang && lang != e->language) return 0;
    if (country && country != e->country) return 0;
    char name[65], file[65];
    for (int i = 0; i < 64; i++) { name[i] = (char)psp_read8(st + 32 + (uint32_t)i); file[i] = (char)psp_read8(st + 96 + (uint32_t)i); }
    name[64] = file[64] = 0;
    if (name[0] && strcmp(name, e->name) != 0) return 0;
    if (file[0] && strcmp(file, e->file) != 0) return 0;
    return 1;
}

/* (lib, SceFontStyle *style, u32 *error) -> registry index. Among fonts that
 * match the style, the one whose size is nearest to the one asked for. */
static void hle_FindOptimumFont(void) {
    uint32_t st = psp_arg(1);
    if (psp_arg(0) != LIB_HANDLE || !st) { set_error(psp_arg(2), SCE_FONT_ERROR_INVALID_LIBID); psp_ret(0); return; }
    float h = rdf(st), v = rdf(st + 4), hres = rdf(st + 8), vres = rdf(st + 12);
    if (hres <= 0) hres = 128.0f;
    if (vres <= 0) vres = 128.0f;
    int best = -1, any = -1;
    float best_d = 1e30f;
    for (int i = 0; i < NREG; i++) {
        if (!g_loaded[i] || !style_matches(st, &k_registry[i])) continue;
        if (any < 0) any = i;
        const font_entry *e = &k_registry[i];
        float d = 1e30f;
        if (h > 0) d = (float)e->hres / 64 * e->hsize / 64 - hres * h;
        else if (v > 0) d = (float)e->vres / 64 * e->vsize / 64 - vres * v;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) best = any >= 0 ? any : 0;
    set_error(psp_arg(2), 0);
    psp_ret((uint32_t)best);
}

/* (lib, index, mode, u32 *error) -> font handle */
static void hle_Open(void) {
    uint32_t idx = psp_arg(1);
    if (psp_arg(0) != LIB_HANDLE) { set_error(psp_arg(3), SCE_FONT_ERROR_INVALID_LIBID); psp_ret(0); return; }
    if (idx >= (uint32_t)NREG || !g_loaded[idx]) {
        set_error(psp_arg(3), SCE_FONT_ERROR_INVALID_PARAMETER);
        psp_ret(0);
        return;
    }
    for (int i = 0; i < MAX_OPEN; i++) {
        if (g_open[i]) continue;
        g_open[i] = (int)idx + 1;
        set_error(psp_arg(3), 0);
        psp_ret(FONT_HANDLE_BASE + (uint32_t)i);
        return;
    }
    set_error(psp_arg(3), SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS);
    psp_ret(0);
}

static void hle_Close(void) {
    uint32_t i = psp_arg(0) - FONT_HANDLE_BASE;
    if (i < MAX_OPEN) g_open[i] = 0;
    psp_ret(0);
}

static void wrf(uint32_t at, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    psp_write32(at, u);
}

static void write_style(uint32_t at, const font_entry *e) {
    for (uint32_t i = 0; i < 168; i++) psp_write8(at + i, 0);
    wrf(at, e->hsize / 64.0f);
    wrf(at + 4, e->vsize / 64.0f);
    wrf(at + 8, e->hres / 64.0f);
    wrf(at + 12, e->vres / 64.0f);
    psp_write16(at + 20, (uint16_t)e->family);
    psp_write16(at + 22, (uint16_t)e->style);
    psp_write16(at + 26, (uint16_t)e->language);
    psp_write16(at + 30, (uint16_t)e->country);
    for (uint32_t i = 0; e->name[i] && i < 63; i++) psp_write8(at + 32 + i, (uint8_t)e->name[i]);
    for (uint32_t i = 0; e->file[i] && i < 63; i++) psp_write8(at + 96 + i, (uint8_t)e->file[i]);
}

/* (font, SceFontInfo *info) */
static void hle_GetFontInfo(void) {
    const pgf *f = open_font(psp_arg(0));
    uint32_t at = psp_arg(1);
    if (!f || !at) { psp_ret(SCE_FONT_ERROR_INVALID_PARAMETER); return; }
    const int32_t iv[10] = { f->max_size[0], f->max_size[1], f->max_ascender, f->max_descender,
                             f->max_leftx, f->max_basey, f->min_centerx, f->max_topy,
                             f->max_adv[0], f->max_adv[1] };
    for (int i = 0; i < 10; i++) {
        psp_write32(at + 4 * (uint32_t)i, (uint32_t)iv[i]);
        wrf(at + 40 + 4 * (uint32_t)i, iv[i] / 64.0f);
    }
    psp_write16(at + 80, f->max_glyph_w);
    psp_write16(at + 82, f->max_glyph_h);
    psp_write32(at + 84, (uint32_t)f->nglyphs);
    psp_write32(at + 88, 0);                         /* shadow map length */
    write_style(at + 92, f->entry);
    psp_write8(at + 260, (uint8_t)f->bpp);
    psp_write8(at + 261, 0); psp_write8(at + 262, 0); psp_write8(at + 263, 0);
    psp_ret(0);
}

/* (font, charCode, SceFontCharInfo *info): 60 bytes, zero for a character the
 * font does not have. */
static void hle_GetCharInfo(void) {
    const pgf *f = open_font(psp_arg(0));
    uint32_t at = psp_arg(2);
    if (!f || !at) { psp_ret(SCE_FONT_ERROR_INVALID_PARAMETER); return; }
    for (uint32_t i = 0; i < 60; i++) psp_write8(at + i, 0);
    const glyph *g = find_glyph(f, (int)(psp_arg(1) & 0xFFFF));
    if (g) {
        const uint32_t v[14] = { (uint32_t)g->w, (uint32_t)g->h, (uint32_t)g->left, (uint32_t)g->top,
                                 (uint32_t)g->dim_w, (uint32_t)g->dim_h,
                                 (uint32_t)g->yadj_h, (uint32_t)(g->yadj_h - g->dim_h),
                                 (uint32_t)g->xadj_h, (uint32_t)g->yadj_h,
                                 (uint32_t)g->xadj_v, (uint32_t)g->yadj_v,
                                 (uint32_t)g->adv_h, (uint32_t)g->adv_v };
        for (uint32_t i = 0; i < 14; i++) psp_write32(at + 4 * i, v[i]);
        psp_write16(at + 56, (uint16_t)g->shadow_flags);
        psp_write16(at + 58, (uint16_t)g->shadow_id);
    }
    psp_ret(0);
}

/* Add coverage into the game's buffer, saturating: a glyph never erases what
 * is already there. Format 0 is two pixels a byte, low nibble first. */
static void add_pixel(uint32_t base, int bpl, int bw, int bh, int x, int y, int v, int fmt) {
    if (x < 0 || y < 0 || x >= bw || y >= bh) return;
    if (fmt == 0) {
        uint32_t at = base + (uint32_t)(y * bpl + x / 2);
        int shift = (x & 1) ? 4 : 0;
        int old = psp_read8(at);
        int p = ((old >> shift) & 0xF) + v;
        if (p > 15) p = 15;
        psp_write8(at, (uint8_t)((old & ~(0xF << shift)) | (p << shift)));
    } else {
        uint32_t at = base + (uint32_t)(y * bpl + x);
        int p = psp_read8(at) + v;
        psp_write8(at, (uint8_t)(p > 255 ? 255 : p));
    }
}

/* (font, charCode, SceFontGlyphImage *image): u32 format, s32 x64, s32 y64,
 * u16 width, u16 height, u16 bytesPerLine, u16 pad, u32 buffer. */
static void hle_GetCharGlyphImage(void) {
    const pgf *f = open_font(psp_arg(0));
    uint32_t img = psp_arg(2);
    if (!f || !img) { psp_ret(SCE_FONT_ERROR_INVALID_PARAMETER); return; }
    psp_ret(0);
    const glyph *g = find_glyph(f, (int)(psp_arg(1) & 0xFFFF));
    if (!g || g->w <= 0 || g->h <= 0) return;
    int dir = g->flags & FLAG_BMP_OVERLAY;
    if (dir != FLAG_H_ROWS && dir != FLAG_V_ROWS) return;

    int fmt = (int)psp_read32(img);
    if (fmt != 0 && fmt != 2) return;            /* hardware draws only 4- and 8-bit */
    int32_t x64 = (int32_t)psp_read32(img + 4), y64 = (int32_t)psp_read32(img + 8);
    int bw = psp_read16(img + 12), bh = psp_read16(img + 14), bpl = psp_read16(img + 16);
    uint32_t buf = psp_read32(img + 20);

    /* Nibble run-length coded coverage, row- or column-major. */
    int npix = g->w * g->h;
    uint8_t *pix = (uint8_t *)calloc((size_t)npix, 1);
    if (!pix) return;
    size_t pos = g->ptr;
    for (int i = 0; i < npix && pos + 8 < f->size * 8;) {
        int nib = (int)take(f->data, &pos, 4);
        int count, value = 0;
        if (nib < 8) { value = (int)take(f->data, &pos, 4); count = nib + 1; }
        else count = 16 - nib;
        for (int k = 0; k < count && i < npix; k++) {
            if (nib >= 8) value = (int)take(f->data, &pos, 4);
            pix[i++] = (uint8_t)value;
        }
    }

    int x = x64 >> 6, y = y64 >> 6, frac = x64 & 0x3F;
    int scale = fmt == 0 ? 1 : 17;
    int x2 = g->w + (frac ? 1 : 0);
    for (int yy = 0; yy < g->h; yy++) {
        for (int xx = 0; xx < x2; xx++) {
            int a = 0, b = 0;
            if (xx - 1 >= 0)
                a = pix[dir == FLAG_H_ROWS ? yy * g->w + xx - 1 : (xx - 1) * g->h + yy] * scale;
            if (xx < g->w)
                b = pix[dir == FLAG_H_ROWS ? yy * g->w + xx : xx * g->h + yy] * scale;
            int v = (a * frac) / 64 + (b * (64 - frac) + 63) / 64;
            if (v) add_pixel(buf, bpl, bw, bh, x + xx, y + yy, v, fmt);
        }
    }
    free(pix);
    g_glyphs_drawn++;
}

void mhfu_libfont_register(const char *font_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", font_dir ? font_dir : "");
    g_tried = 0;
    memset(g_open, 0, sizeof g_open);

    psp_hle_register(0x67F17ED7, "sceLibFont", "sceFontNewLib",            hle_NewLib);
    psp_hle_register(0x574B6FBC, "sceLibFont", "sceFontDoneLib",           hle_DoneLib);
    psp_hle_register(0x27F6E642, "sceLibFont", "sceFontGetNumFontList",    hle_GetNumFontList);
    psp_hle_register(0x099EF33C, "sceLibFont", "sceFontFindOptimumFont",   hle_FindOptimumFont);
    psp_hle_register(0xA834319D, "sceLibFont", "sceFontOpen",              hle_Open);
    psp_hle_register(0x3AEA8CB6, "sceLibFont", "sceFontClose",             hle_Close);
    psp_hle_register(0x0DA7535E, "sceLibFont", "sceFontGetFontInfo",       hle_GetFontInfo);
    psp_hle_register(0xDCC80C2F, "sceLibFont", "sceFontGetCharInfo",       hle_GetCharInfo);
    psp_hle_register(0x980F4895, "sceLibFont", "sceFontGetCharGlyphImage", hle_GetCharGlyphImage);
}
