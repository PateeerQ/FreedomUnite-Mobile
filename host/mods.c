/* MHFU-Android -- see mods.h. The cheat engine follows PPSSPP's CWCheat one
 * (Core/CwCheat.cpp): the same code types and the same meaning. */
#include "mods.h"

#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include "net/client.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#define mkdir_(p) mkdir(p, 0755)
#endif

/* ---- widescreen ---- */

static volatile int g_wide = -1;            /* -1: MHFU_WIDESCREEN not read yet */

int mhfu_mod_widescreen(void) {
    if (g_wide < 0) {
        const char *e = getenv("MHFU_WIDESCREEN");
        mhfu_mod_set_widescreen(e && atoi(e) != 0);
    }
    return g_wide;
}

void mhfu_mod_set_widescreen(int on) {
    g_wide = on ? 1 : 0;
    psp_render_gl_set_stretch(g_wide);
}

unsigned mhfu_mod_aspect(unsigned original) {
    if (!mhfu_mod_widescreen()) return original;
    int w = 0, h = 0;
    mhfu_window_size(&w, &h);
    if (w <= 0 || h <= 0) return original;
    union { float f; unsigned u; } a;
    a.f = (float)w / (float)h;
    return a.u;
}

/* ---- cheats ---- */

typedef struct { uint32_t a, b; } cline;
typedef struct {
    char name[96];
    int  first, n;                          /* its lines in g_lines */
} cheat;

#define MAX_CHEATS 512
#define MAX_LINES  16384
static cheat  g_cheats[MAX_CHEATS];
static cline  g_lines[MAX_LINES];
static int    g_ncheats, g_nlines, g_active;
static char   g_path[1024];
static long long g_mtime = -1;
static char   g_root[900];

static long long file_mtime(void) {
    struct stat st;
    return stat(g_path, &st) == 0 ? (long long)st.st_mtime : -2;
}

/* A cheat that writes the EBOOT's code (its first-line types 0-2, 4 and 8
 * write where they say): recompiled code would never see it. */
static int writes_code(const cheat *c) {
    for (int i = 0; i < c->n; i++) {
        const cline *l = &g_lines[c->first + i];
        const unsigned t = l->a >> 28;
        const uint32_t off = l->a & 0x0FFFFFFF;
        if ((t <= 2 || t == 4 || t == 8) && off >= MHFU_CHEAT_CODE_LO && off < MHFU_CHEAT_CODE_HI) return 1;
        if (t == 4 || t == 8 || t == 3 || t == 5) i++;      /* their second line is not a code line */
    }
    return 0;
}

static void load(void) {
    g_ncheats = g_nlines = g_active = 0;
    FILE *f = fopen(g_path, "r");
    if (!f) return;
    char line[512];
    int game_ok = 1, on = 0, skipped = 0;
    cheat *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t' || (unsigned char)*s == 0xEF || (unsigned char)*s == 0xBB || (unsigned char)*s == 0xBF) s++;
        if (s[0] != '_' || !s[1]) continue;
        if (s[1] == 'S') {                   /* a game: only ours (a file of one game counts as ours) */
            game_ok = strstr(s, "ULES-01213") || strstr(s, "ULES01213");
            cur = NULL;
        } else if (s[1] == 'C' && game_ok) {
            cur = NULL;
            on = s[2] >= '1' && s[2] <= '9';
            if (on && g_ncheats < MAX_CHEATS) {
                cur = &g_cheats[g_ncheats++];
                const char *name = s[3] ? s + 4 : "";
                snprintf(cur->name, sizeof cur->name, "%s", name);
                cur->name[strcspn(cur->name, "\r\n")] = 0;
                cur->first = g_nlines;
                cur->n = 0;
            }
        } else if (s[1] == 'L' && cur && g_nlines < MAX_LINES) {
            unsigned a, b;
            if (sscanf(s + 2, "%x %x", &a, &b) == 2) {
                g_lines[g_nlines++] = (cline){ a, b };
                cur->n++;
            }
        }
    }
    fclose(f);
    /* What runs: cheats with lines that do not write code. */
    int w = 0;
    for (int i = 0; i < g_ncheats; i++) {
        if (!g_cheats[i].n) continue;
        if (writes_code(&g_cheats[i])) {
            printf("cheats: \"%s\" writes the game's code, which is recompiled here: skipped\n", g_cheats[i].name);
            skipped++;
            continue;
        }
        g_cheats[w++] = g_cheats[i];
    }
    g_ncheats = g_active = w;
    for (int i = 0; i < g_ncheats; i++) printf("cheats: on: %s\n", g_cheats[i].name);
    if (g_ncheats || skipped) printf("cheats: %d on, %d skipped (%s)\n", g_ncheats, skipped, g_path);
    fflush(stdout);
}

void mhfu_cheats_init(const char *root) {
    snprintf(g_path, sizeof g_path, "%s/cheats/ULES01213.ini", root);
    snprintf(g_root, sizeof g_root, "%s", root);
    g_mtime = file_mtime();
    load();
}

int mhfu_cheats_active(void) { return g_active; }

/* ---- running them ---- */

static uint32_t addr_of(uint32_t v) { return (v + 0x08800000u) & 0x3FFFFFFFu; }
static int valid(uint32_t a, uint32_t n) { return a >= 0x08000000u && a + n <= 0x0A000000u && a + n >= a; }

static uint32_t rd(uint32_t a, int sz) {
    return sz == 1 ? psp_read8(a) : sz == 2 ? psp_read16(a) : psp_read32(a);
}
static void wr(uint32_t a, int sz, uint32_t v) {
    if (!valid(a, (uint32_t)sz)) return;
    if (sz == 1) psp_write8(a, (uint8_t)v);
    else if (sz == 2) psp_write16(a, (uint16_t)v);
    else psp_write32(a, v);
}
static int test(int op, int x, int y) {
    return op == 0 ? x == y : op == 1 ? x != y : op == 2 ? x < y : x > y;
}

static void run(const cheat *c, uint32_t buttons) {
    const cline *L = &g_lines[c->first];
    const int n = c->n;
    for (int i = 0; i < n;) {
        const uint32_t p1 = L[i].a, arg = L[i].b;
        i++;
        const unsigned type = p1 >> 28;
        uint32_t a;
        switch (type) {
        case 0x0:                                            /* 8-bit write (up to 4 bytes) */
            a = addr_of(p1 & 0x0FFFFFFF);
            wr(a, (arg & 0xFFFF0000) ? 4 : (arg & 0xFF00) ? 2 : 1, arg);
            break;
        case 0x1: wr(addr_of(p1 & 0x0FFFFFFF), 2, arg); break;
        case 0x2: wr(addr_of(p1 & 0x0FFFFFFF), 4, arg); break;
        case 0x3: {                                          /* increment / decrement */
            a = addr_of(arg & 0x0FFFFFFF);
            const unsigned k = (p1 >> 20) & 0xF;
            int sz = 0; uint32_t v = 0; int sub = 0;
            if (k == 1 || k == 2) { sz = 1; v = p1 & 0xFF; sub = k == 2; }
            else if (k == 3 || k == 4) { sz = 2; v = p1 & 0xFFFF; sub = k == 4; }
            else if ((k == 5 || k == 6) && i < n) { sz = 4; v = L[i++].a; sub = k == 6; }
            else return;
            if (valid(a, (uint32_t)sz)) wr(a, sz, sub ? rd(a, sz) - v : rd(a, sz) + v);
            break;
        }
        case 0x4: case 0x8: {                                /* multi-write */
            if (i >= n) return;
            const cline l2 = L[i++];
            a = addr_of(p1 & 0x0FFFFFFF);
            int sz = 4;
            uint32_t v = l2.a, step = (arg & 0xFFFF) * 4;
            if (type == 0x8) {
                const int is8 = (l2.a & 0xFFFF0000) == 0;
                sz = is8 ? 1 : 2;
                v = is8 ? (l2.a & 0xFF) : (l2.a & 0xFFFF);
                step = (arg & 0xFFFF) * (uint32_t)sz;
            }
            const uint32_t count = arg >> 16;
            if ((uint64_t)count * step + (uint32_t)sz >= 24u * 1024 * 1024) break;
            for (uint32_t k = 0; k < count; k++, a += step, v += l2.b) wr(a, sz, v);
            break;
        }
        case 0x5: {                                          /* copy bytes */
            if (i >= n) return;
            const uint32_t src = addr_of(p1 & 0x0FFFFFFF), dst = addr_of(L[i++].a & 0x0FFFFFFF);
            if (valid(src, arg) && valid(dst, arg))
                for (uint32_t k = 0; k < arg; k++) psp_write8(dst + k, psp_read8(src + k));
            break;
        }
        case 0x6: {                                          /* pointer commands */
            if (i >= n) return;
            const cline l2 = L[i++];
            const int count = (int)(l2.a & 0xFFFF) - 1;
            if (count < 0 || i + count > n) return;
            a = addr_of(p1 & 0x0FFFFFFF);
            const int offset = (int)l2.b, base_off = ((int)l2.a >> 20) * 4;
            int ptype = (int)((l2.a >> 16) & 0xF);
            uint32_t val = arg;
            if (!valid(a + (uint32_t)base_off, 4)) { i += count; break; }
            uint32_t base = psp_read32(a + (uint32_t)base_off);
            for (int k = 0; k < count; k++) {
                const cline l = L[i++];
                const unsigned t = l.a >> 28;
                if (t == 0x1) {
                    if (valid(a, 4)) {
                        const uint32_t s2 = psp_read32(a) + (uint32_t)offset, d2 = base + (l.a & 0x0FFFFFFF);
                        if (valid(s2, val) && valid(d2, val))
                            for (uint32_t m = 0; m < val; m++) psp_write8(d2 + m, psp_read8(s2 + m));
                        ptype = -1;
                    }
                } else if (t == 0x2 || t == 0x3) {
                    int walk = (int)(l.a & 0x0FFFFFFF);
                    if (t == 0x3) walk = -walk;
                    if (valid(base + (uint32_t)walk, 4)) {
                        base = psp_read32(base + (uint32_t)walk);
                        const unsigned t2 = l.b >> 28;
                        if (t2 == 0x2 || t2 == 0x3) {
                            walk = (int)(l.b & 0x0FFFFFFF);
                            if (t2 == 0x3) walk = -walk;
                            if (valid(base + (uint32_t)walk, 4)) base = psp_read32(base + (uint32_t)walk);
                        }
                    }
                } else if (t == 0x9) {
                    base += l.a & 0x0FFFFFFF;
                    val += l.b;
                }
            }
            switch (ptype) {
            case 0: wr(base + (uint32_t)offset, 1, val); break;
            case 1: wr(base + (uint32_t)offset, 2, val); break;
            case 2: wr(base + (uint32_t)offset, 4, val); break;
            case 3: wr(base - (uint32_t)offset, 1, val); break;
            case 4: wr(base - (uint32_t)offset, 2, val); break;
            case 5: wr(base - (uint32_t)offset, 4, val); break;
            default: break;
            }
            break;
        }
        case 0x7: {                                          /* boolean */
            a = addr_of(p1 & 0x0FFFFFFF);
            const unsigned k = arg >> 16;
            const int sz = (k & 1) ? 2 : 1;
            const uint32_t v = sz == 1 ? (arg & 0xFF) : (arg & 0xFFFF);
            if (k > 5 || !valid(a, (uint32_t)sz)) break;
            const uint32_t m = rd(a, sz);
            wr(a, sz, k <= 1 ? (m | v) : k <= 3 ? (m & v) : (m ^ v));
            break;
        }
        case 0xB: break;                                     /* delay: not supported (PPSSPP neither) */
        case 0xC:                                            /* 32-bit equal, or stop */
            a = addr_of(p1 & 0x0FFFFFFF);
            if (valid(a, 4) && psp_read32(a) != arg) return;
            break;
        case 0xD: {
            const unsigned k = arg >> 28;
            if (k == 0x0 || k == 0x2) {                      /* next line skip test */
                a = addr_of(p1 & 0x0FFFFFFF);
                const int sz = k == 0x2 ? 1 : 2;
                const uint32_t v = sz == 1 ? (arg & 0xFF) : (arg & 0xFFFF);
                const unsigned op = (arg >> 20) & 0xF;
                if (op > 3) return;
                if (!(valid(a, (uint32_t)sz) && test((int)op, (int)rd(a, sz), (int)v))) i += 1;
            } else if (k == 0x1 || k == 0x3) {               /* joker: buttons (not) held */
                const uint32_t want = arg & 0x0FFFFFFF;
                const int held = (buttons & want) == want;
                if (k == 0x1 ? !held : held) i += (int)(p1 & 0xFF) + 1;
            } else if (k >= 0x4 && k <= 0x7) {               /* address against address */
                if (i >= n) return;
                const cline l2 = L[i++];
                const int sz = 1 << (l2.b & 0xF);
                a = addr_of(p1 & 0x0FFFFFFF);
                const uint32_t b2 = addr_of(arg & 0x0FFFFFFF);
                const int ok = valid(a, (uint32_t)sz) && valid(b2, (uint32_t)sz) &&
                               test((int)(k - 4), (int)rd(a, sz), (int)rd(b2, sz));
                if (!ok) i += (int)l2.a;
            } else return;
            break;
        }
        case 0xE: {                                          /* multiple line skip test */
            a = addr_of(arg & 0x0FFFFFFF);
            const int is8 = (p1 >> 24) == 0xE1;
            const int sz = is8 ? 1 : 2;
            const uint32_t v = is8 ? (p1 & 0xFF) : (p1 & 0xFFFF);
            const unsigned op = arg >> 28;
            if (op > 3) return;
            const int skip = (int)((p1 >> 16) & (is8 ? 0xFF : 0xFFF));
            if (!(valid(a, (uint32_t)sz) && test((int)op, (int)rd(a, sz), (int)v))) i += skip;
            break;
        }
        default:
            return;                                          /* not a code type we know: the rest is skipped */
        }
    }
}

void mhfu_cheats_frame(uint32_t buttons) {
    if (!g_path[0]) return;
    static unsigned frame;
    /* MHFU_MEMDUMP_AT=4500,6000: memory dumps at those game frames (scripted runs). */
    static const char *dump_at = (const char *)1;
    if (dump_at == (const char *)1) dump_at = getenv("MHFU_MEMDUMP_AT");
    if (dump_at)
        for (const char *p = dump_at; *p; p++)
            if ((p == dump_at || p[-1] == ',') && (unsigned)atoi(p) == frame + 1) mhfu_mods_dump(NULL, 0);
    if (++frame % 30 == 0) {                                 /* changed (the Mods page): read again */
        const long long m = file_mtime();
        if (m != g_mtime) { g_mtime = m; load(); }
    }
    if (!g_ncheats) return;
    static int paused;
    if (mhn_client_online()) {                               /* not in rooms: the others do not have them */
        if (!paused) { printf("cheats: paused while in a room\n"); paused = 1; }
        return;
    }
    if (paused) { printf("cheats: on again\n"); paused = 0; }
    for (int i = 0; i < g_ncheats; i++) run(&g_cheats[i], buttons);
}

int mhfu_mods_dump(char *path_out, int cap) {
    char dir[1000], path[1100], stamp[32];
    snprintf(dir, sizeof dir, "%s/../memdump", g_root[0] ? g_root : "work/root");
    mkdir_(dir);
    const time_t t = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(path, sizeof path, "%s/dump-%s.bin", dir, stamp);
    const uint8_t *m = (const uint8_t *)psp_mem_ptr(0x08800000u, 0x01800000u);
    FILE *f = m ? fopen(path, "wb") : NULL;
    if (!f) return -1;
    const size_t n = fwrite(m, 1, 0x01800000u, f);
    fclose(f);
    printf("mods: memory 08800000-0A000000 saved to %s\n", path);
    fflush(stdout);
    if (path_out) snprintf(path_out, (size_t)cap, "%s", path);
    return n == 0x01800000u ? 0 : -1;
}
