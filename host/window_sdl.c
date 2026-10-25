/* MHFU-Android -- the window through SDL3: window.h for Android and Linux,
 * and for Windows when built with MHFU_SDL_WINDOW (window.c is the Win32 one,
 * with the Multiplayer menu and dialogs this one does not have yet).
 *
 * SDL wants its window and events on the program's main thread (on Android
 * it insists), so mhfu_window_main runs SDL there and the game on a thread of
 * its own. The game's calls here (mhfu_window_start, _gl, _ask_text, _notify)
 * hand their work to that loop.
 *
 * The context is GL 3.3 core on the desktop and GLES 3.0 on Android
 * (MHFU_GLES=1 asks for GLES on the desktop too, to try the GLES shaders).
 * psprecomp's render thread creates and uses it. The overlay -- the FPS line,
 * achievement toasts, the text box for the game's keyboard -- is drawn here
 * into an image with stb_truetype and handed to psp_render_gl_overlay. */
#include "window.h"
#include "achievements.h"

#if MHFU_SDL_WINDOW

#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "gamepad.h"
#include "input.h"
#include "keys.h"
#include "lobby.h"
#include "stats.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define PSP_W 480
#define PSP_H 272

static SDL_Window      *g_window;
static SDL_GLContext    g_ctx;
static int              g_scale = 2, g_fullscreen, g_out_w, g_out_h, g_gles;
static SDL_AtomicInt    g_buttons, g_stick, g_closed;
static void           (*g_on_close)(void);
static int              g_gl;
static float            g_ui = 1;               /* the display's scale, for the overlay's sizes */
static Uint32           g_ev_window, g_ev_wake;   /* our own event types */
static int              g_show_fps = 1, g_show_fps_init = 1;
static SDL_Mutex       *g_lock;
static SDL_Condition   *g_cond;
static int              g_window_state;          /* 0 asked for, 1 made, -1 failed */

/* ---- text: stb_truetype over a system font ---------------------------------------- */

static stbtt_fontinfo g_font;
static unsigned char *g_font_data;
static int            g_font_ok;

static void font_init(void) {
    static const char *const candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0] && !g_font_ok; i++) {
        size_t n = 0;
        void *d = SDL_LoadFile(candidates[i], &n);
        if (!d) continue;
        if (stbtt_InitFont(&g_font, (const unsigned char *)d, stbtt_GetFontOffsetForIndex((const unsigned char *)d, 0))) {
            g_font_data = (unsigned char *)d;
            g_font_ok = 1;
        } else SDL_free(d);
    }
    if (!g_font_ok) fprintf(stderr, "window: no system font found; the overlay has no text\n");
}

typedef struct { int cp, px, w, h, xoff, yoff, adv; unsigned char *bmp; } glyph;
#define GLYPHS 768
static glyph g_glyphs[GLYPHS];
static int   g_nglyphs;

static const glyph *glyph_of(int cp, int px) {
    for (int i = 0; i < g_nglyphs; i++) if (g_glyphs[i].cp == cp && g_glyphs[i].px == px) return &g_glyphs[i];
    if (g_nglyphs == GLYPHS) {                       /* full: start over */
        for (int i = 0; i < g_nglyphs; i++) stbtt_FreeBitmap(g_glyphs[i].bmp, NULL);
        g_nglyphs = 0;
    }
    glyph *g = &g_glyphs[g_nglyphs++];
    const float s = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
    g->cp = cp;
    g->px = px;
    g->adv = (int)lroundf(adv * s);
    g->bmp = stbtt_GetCodepointBitmap(&g_font, s, s, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* The next code point of a UTF-8 string. */
static int utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s++, n = 0;
    if (c >= 0xF0) { c &= 0x07; n = 3; } else if (c >= 0xE0) { c &= 0x0F; n = 2; } else if (c >= 0xC0) { c &= 0x1F; n = 1; }
    while (n-- && (*s & 0xC0) == 0x80) c = c << 6 | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}

static int text_width(const char *t, int px) {
    if (!g_font_ok) return 0;
    int w = 0;
    while (*t) w += glyph_of(utf8_next(&t), px)->adv;
    return w;
}

/* ---- the overlay canvas: 0xAABBGGRR, what psp_render_gl_overlay takes -------------- */

typedef struct { uint32_t *px; int w, h; } canvas;

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

static int g_clip0, g_clip1 = 1 << 30;     /* rows drawn: [g_clip0, g_clip1) */

/* Source-over, on a canvas whose pixels may still be transparent. */
static void blend(canvas *c, int x, int y, uint32_t col, int cov) {
    if (x < 0 || y < g_clip0 || x >= c->w || y >= c->h || y >= g_clip1) return;
    const int sa = (int)(col >> 24) * cov / 255;
    if (!sa) return;
    uint32_t *d = &c->px[y * c->w + x];
    const int da = (int)(*d >> 24), oa = sa + da * (255 - sa) / 255;
    if (!oa) return;
    int out = 0;
    for (int k = 0; k < 24; k += 8) {
        const int s = (int)(col >> k & 0xFF), dd = (int)(*d >> k & 0xFF);
        out |= ((s * sa + dd * da * (255 - sa) / 255) / oa) << k;
    }
    *d = (uint32_t)out | (uint32_t)oa << 24;
}

static void fill(canvas *c, int x, int y, int w, int h, uint32_t col) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) blend(c, i, j, col, 255);
}

static void frame_rect(canvas *c, int x, int y, int w, int h, int t, uint32_t col) {
    fill(c, x, y, w, t, col); fill(c, x, y + h - t, w, t, col);
    fill(c, x, y, t, h, col); fill(c, x + w - t, y, t, h, col);
}

static void diamond(canvas *c, int cx, int cy, int r, uint32_t col) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (abs(i) + abs(j) <= r) blend(c, cx + i, cy + j, col, 255);
}

/* Text on a baseline, cut with "..." at max_w (0: no limit). */
static void text(canvas *c, int x, int baseline, int px, uint32_t col, const char *t, int max_w) {
    if (!g_font_ok) return;
    const int dots = max_w ? text_width("...", px) : 0;
    const int whole = text_width(t, px);
    int pen = x;
    while (*t) {
        const char *before = t;
        const glyph *g = glyph_of(utf8_next(&t), px);
        if (max_w && whole > max_w && pen + g->adv > x + max_w - dots) { t = before; break; }
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) blend(c, pen + g->xoff + i, baseline + g->yoff + j, col, g->bmp[j * g->w + i]);
        pen += g->adv;
    }
    if (*t) text(c, pen, baseline, px, col, "...", 0);
}

/* Up to `lines` lines, broken at spaces. */
static void text_wrapped(canvas *c, int x, int baseline, int px, int line_h, uint32_t col, const char *t, int max_w, int lines) {
    char line[512];
    for (int l = 0; l < lines && *t; l++) {
        int n = 0, last_space = -1;
        const char *p = t;
        while (*p) {
            const char *q = p;
            utf8_next(&q);
            if ((size_t)(n + (q - p)) >= sizeof line) break;
            memcpy(line + n, p, (size_t)(q - p));
            line[n + (q - p)] = 0;
            if (text_width(line, px) > max_w) break;
            if (*p == ' ') last_space = n;
            n += (int)(q - p);
            p = q;
        }
        if (*p && last_space > 0 && l + 1 < lines) { n = last_space; p = t + n + 1; }
        line[n] = 0;
        text(c, x, baseline + l * line_h, px, col, l + 1 == lines && *p ? t : line, max_w);
        t = p;
    }
}

/* Anti-aliased shapes: coverage from the distance to the edge. */
static int cov(float d) { return d <= -0.5f ? 255 : d >= 0.5f ? 0 : (int)((0.5f - d) * 255); }

static void disc(canvas *c, float cx, float cy, float r, uint32_t col) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r;
            const int k = cov(d);
            if (k) blend(c, x, y, col, k);
        }
}

static void ring(canvas *c, float cx, float cy, float r, float t, uint32_t col) {
    for (int y = (int)(cy - r - t); y <= (int)(cy + r + t); y++)
        for (int x = (int)(cx - r - t); x <= (int)(cx + r + t); x++) {
            const float d = fabsf(sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r) - t / 2;
            const int k = cov(d);
            if (k) blend(c, x, y, col, k);
        }
}

static void segment(canvas *c, float x0, float y0, float x1, float y1, float t, uint32_t col) {
    const float minx = fminf(x0, x1) - t, maxx = fmaxf(x0, x1) + t, miny = fminf(y0, y1) - t, maxy = fmaxf(y0, y1) + t;
    const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++)
        for (int x = (int)minx; x <= (int)maxx; x++) {
            const float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float u = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            u = u < 0 ? 0 : u > 1 ? 1 : u;
            const float ex = px - u * dx, ey = py - u * dy;
            const int k = cov(sqrtf(ex * ex + ey * ey) - t / 2);
            if (k) blend(c, x, y, col, k);
        }
}

static void round_rect(canvas *c, float x, float y, float w, float h, float r, uint32_t col, float border, uint32_t bcol) {
    for (int j = (int)y; j < (int)(y + h); j++)
        for (int i = (int)x; i < (int)(x + w); i++) {
            const float qx = fmaxf(fabsf(i + 0.5f - (x + w / 2)) - (w / 2 - r), 0);
            const float qy = fmaxf(fabsf(j + 0.5f - (y + h / 2)) - (h / 2 - r), 0);
            const float d = sqrtf(qx * qx + qy * qy) - r;
            const int k = cov(d);
            if (!k) continue;
            const int inner = border > 0 ? cov(d + border) : 255;
            if (inner) blend(c, i, j, col, inner * k / 255);
            if (border > 0 && k > inner) blend(c, i, j, bcol, k - inner);
        }
}

static void text_centered(canvas *c, float cx, float cy, int px, uint32_t col, const char *t) {
    text(c, (int)(cx - text_width(t, px) / 2.0f), (int)(cy + px * 0.36f), px, col, t, 0);
}

/* ---- touch controls ------------------------------------------------------------------
 *
 * A PSP laid over the screen in groups: the analog stick, the d-pad, the face
 * buttons, L, R, Select, Start and the menu button. Each group has a centre
 * and a size; the defaults put the d-pad and stick on the left, the face
 * buttons on the right, L and R in the top corners, Select and Start at the
 * bottom and the menu button at the top. Sizes are in units of the screen's
 * height, so they stay round on any phone. The layout editor (the drawer's
 * "Edit button layout") moves and resizes each group; positions are kept as
 * fractions of the screen, with the opacity and the other settings, in
 * settings.ini in SDL's preferences folder.
 *
 * Every finger is tracked: the stick follows its finger, the d-pad takes 8
 * directions, and a finger slides from button to button. The drawer turns the
 * controls on, off or automatic -- off while a controller is connected. */

enum { T_NONE, T_STICK, T_DPAD, T_CROSS, T_CIRCLE, T_SQUARE, T_TRIANGLE, T_L, T_R, T_SELECT, T_START, T_MENU, T_EDGE, T_RSTICK };
enum { VPAD_AUTO, VPAD_ON, VPAD_OFF };
enum { G_STICK, G_DPAD, G_FACE, G_L, G_R, G_SELECT, G_START, G_MENU, G_RSTICK, G_COUNT };
static const char *const k_group_name[G_COUNT] = { "stick", "dpad", "face", "l", "r", "select", "start", "menu", "rstick" };

static int     g_vpad_mode = VPAD_AUTO;
static int     g_vpad_alpha = 100;                    /* percent */
static int     g_menu_open;
static int     g_edit;                                 /* the layout editor is open */
static int     g_edit_sel = -1;                        /* the group being edited */
static uint32_t g_touch_buttons;
static int     g_touch_ax = -1, g_touch_ay = -1;         /* the stick, 0-255; -1 not touched */
static float   g_touch_rx, g_touch_ry;                  /* the right stick's knob, -1..1 */
static int     g_touch_rheld;
static SDL_AtomicInt g_touch_btn_atomic, g_touch_stick_atomic;  /* for the game thread */
static SDL_AtomicInt g_remake;                       /* the surface changed: the render thread rebinds */

/* A group's place when it has been moved: its centre as fractions of the
 * screen, and its size against the default. */
typedef struct { int set; float cx, cy, k; } placed;
static placed g_place[G_COUNT];

typedef struct { SDL_FingerID id; int elem; float x, y; } finger;
#define FINGERS 10
static finger g_fingers[FINGERS];

static int vpad_visible(void) {
    if (g_edit) return 1;
    if (g_vpad_mode == VPAD_ON) return 1;
    if (g_vpad_mode == VPAD_OFF) return 0;
#ifdef __ANDROID__
    return !mhfu_gamepad_connected();
#else
    return 0;                                          /* the desktop has a keyboard */
#endif
}

static char g_settings_path[1024];
static void settings_load(void) {
    char *pref = SDL_GetPrefPath("PateeerQ", "MHFU");
    if (!pref) return;
    snprintf(g_settings_path, sizeof g_settings_path, "%ssettings.ini", pref);
    SDL_free(pref);
    FILE *f = fopen(g_settings_path, "r");
    char line[160];
    while (f && fgets(line, sizeof line, f)) {
        int v;
        float x, y, k;
        char name[32];
        if (sscanf(line, "virtual_gamepad=%d", &v) == 1 && v >= 0 && v <= 2) g_vpad_mode = v;
        if (sscanf(line, "show_fps=%d", &v) == 1) g_show_fps_init = v;
        if (sscanf(line, "button_opacity=%d", &v) == 1 && v >= 10 && v <= 100) g_vpad_alpha = v;
        if (sscanf(line, "layout_%31[a-z]=%f,%f,%f", name, &x, &y, &k) == 4)
            for (int g = 0; g < G_COUNT; g++)
                if (!strcmp(name, k_group_name[g])) g_place[g] = (placed){ 1, x, y, k };
    }
    if (f) fclose(f);
}
static void settings_save(void) {
    FILE *f = g_settings_path[0] ? fopen(g_settings_path, "w") : NULL;
    if (!f) return;
    fprintf(f, "# MHFU: the in-game menu's settings\nvirtual_gamepad=%d\nshow_fps=%d\nbutton_opacity=%d\n",
            g_vpad_mode, g_show_fps_init, g_vpad_alpha);
    for (int g = 0; g < G_COUNT; g++)
        if (g_place[g].set)
            fprintf(f, "layout_%s=%.4f,%.4f,%.3f\n", k_group_name[g], g_place[g].cx, g_place[g].cy, g_place[g].k);
    fclose(f);
}

/* Where each group is on a w x h screen: its centre and its size factor.
 * The shapes' own sizes follow from the screen's height times that factor. */
typedef struct { float w, h, x[G_COUNT], y[G_COUNT], k[G_COUNT]; } layout;
static layout layout_for(float w, float h) {
    layout L;
    L.w = w;
    L.h = h;
    const float def[G_COUNT][2] = {
        { 0.24f * h, h - 0.26f * h }, { 0.24f * h, 0.40f * h }, { w - 0.25f * h, 0.50f * h },
        { 0.20f * h, 0.09f * h }, { w - 0.20f * h, 0.09f * h },
        { w / 2 - 0.13f * h, h - 0.0725f * h }, { w / 2 + 0.13f * h, h - 0.0725f * h }, { w / 2, 0.08f * h },
        { w - 0.58f * h, h - 0.20f * h },
    };
    for (int g = 0; g < G_COUNT; g++) {
        L.x[g] = g_place[g].set ? g_place[g].cx * w : def[g][0];
        L.y[g] = g_place[g].set ? g_place[g].cy * h : def[g][1];
        L.k[g] = g_place[g].set ? g_place[g].k : 1.0f;
    }
    return L;
}

/* Each group's sizes, from the screen's height and its factor. */
static float stick_r(const layout *L) { return 0.15f * L->h * L->k[G_STICK]; }
static float rstick_r(const layout *L) { return 0.13f * L->h * L->k[G_RSTICK]; }
static float dpad_r(const layout *L)  { return 0.15f * L->h * L->k[G_DPAD]; }
static float face_o(const layout *L)  { return 0.12f * L->h * L->k[G_FACE]; }
static float face_r(const layout *L)  { return 0.068f * L->h * L->k[G_FACE]; }
static float menu_r(const layout *L)  { return 0.055f * L->h * L->k[G_MENU]; }
/* L, R, Select and Start are pills: half their width and height. */
static void pill_size(const layout *L, int g, float *hw, float *hh) {
    const int shoulder = g == G_L || g == G_R;
    *hw = (shoulder ? 0.15f : 0.10f) * L->h * L->k[g];
    *hh = (shoulder ? 0.05f : 0.0375f) * L->h * L->k[g];
}

/* A group's bounds, for the editor. */
static void group_box(const layout *L, int g, float *x0, float *y0, float *x1, float *y1) {
    float rx, ry;
    switch (g) {
    case G_STICK: rx = ry = stick_r(L); break;
    case G_RSTICK: rx = ry = rstick_r(L); break;
    case G_DPAD: rx = ry = dpad_r(L); break;
    case G_FACE: rx = ry = face_o(L) + face_r(L); break;
    case G_MENU: rx = ry = menu_r(L); break;
    default: pill_size(L, g, &rx, &ry); break;
    }
    *x0 = L->x[g] - rx; *y0 = L->y[g] - ry; *x1 = L->x[g] + rx; *y1 = L->y[g] + ry;
}

static float dist(float ax, float ay, float bx, float by) { return sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }

/* The control under (x, y), in window pixels. Generous: a thumb is wide. */
static int hit(const layout *L, float x, float y) {
    if (dist(x, y, L->x[G_MENU], L->y[G_MENU]) < menu_r(L) * 1.4f) return T_MENU;
    static const int pills[4] = { G_L, G_R, G_SELECT, G_START }, pill_elem[4] = { T_L, T_R, T_SELECT, T_START };
    for (int i = 0; i < 4; i++) {
        float hw, hh;
        pill_size(L, pills[i], &hw, &hh);
        if (fabsf(x - L->x[pills[i]]) < hw * 1.1f && fabsf(y - L->y[pills[i]]) < hh * 1.6f) return pill_elem[i];
    }
    const float fo = face_o(L);
    const struct { float ox, oy; int e; } face[4] = {
        { 0, fo, T_CROSS }, { fo, 0, T_CIRCLE }, { -fo, 0, T_SQUARE }, { 0, -fo, T_TRIANGLE } };
    int best = T_NONE;
    float bd = face_r(L) * 1.5f;
    for (int i = 0; i < 4; i++) {
        const float d = dist(x, y, L->x[G_FACE] + face[i].ox, L->y[G_FACE] + face[i].oy);
        if (d < bd) { bd = d; best = face[i].e; }
    }
    if (best) return best;
    if (dist(x, y, L->x[G_DPAD], L->y[G_DPAD]) < dpad_r(L) * 1.25f) return T_DPAD;
    if (dist(x, y, L->x[G_RSTICK], L->y[G_RSTICK]) < rstick_r(L) * 1.3f) return T_RSTICK;
    if (dist(x, y, L->x[G_STICK], L->y[G_STICK]) < stick_r(L) * 1.6f) return T_STICK;
    return T_NONE;
}

static uint32_t elem_button(int e) {
    switch (e) {
    case T_CROSS: return PSP_CTRL_CROSS;     case T_CIRCLE: return PSP_CTRL_CIRCLE;
    case T_SQUARE: return PSP_CTRL_SQUARE;   case T_TRIANGLE: return PSP_CTRL_TRIANGLE;
    case T_L: return PSP_CTRL_LTRIGGER;      case T_R: return PSP_CTRL_RTRIGGER;
    case T_SELECT: return PSP_CTRL_SELECT;   case T_START: return PSP_CTRL_START;
    default: return 0;
    }
}

/* The PSP state of every finger, into what the game reads. */
static void touch_update(float w, float h) {
    const layout L = layout_for(w, h);
    uint32_t b = 0;
    int ax = -1, ay = -1, rheld = 0;
    float rx = 0, ry = 0;
    for (int i = 0; i < FINGERS; i++) {
        const finger *f = &g_fingers[i];
        if (!f->elem) continue;
        if (f->elem == T_STICK) {
            const float r = stick_r(&L);
            float dx = (f->x - L.x[G_STICK]) / r, dy = (f->y - L.y[G_STICK]) / r;
            const float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) { dx /= m; dy /= m; }
            ax = (int)(128 + dx * 127);
            ay = (int)(128 + dy * 127);
        } else if (f->elem == T_RSTICK) {
            const float r = rstick_r(&L);
            float dx = (f->x - L.x[G_RSTICK]) / r, dy = (f->y - L.y[G_RSTICK]) / r;
            const float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) { dx /= m; dy /= m; }
            rx = dx;
            ry = dy;
            rheld = 1;
            if (m > 0.4f) {
                const float a = atan2f(dy, dx) * 180 / 3.14159265f;
                if (a > -67.5f && a < 67.5f) b |= PSP_CTRL_RIGHT;
                if (a > 22.5f && a < 157.5f) b |= PSP_CTRL_DOWN;
                if (a > 112.5f || a < -112.5f) b |= PSP_CTRL_LEFT;
                if (a > -157.5f && a < -22.5f) b |= PSP_CTRL_UP;
            }
        } else if (f->elem == T_DPAD) {
            const float dx = f->x - L.x[G_DPAD], dy = f->y - L.y[G_DPAD];
            if (sqrtf(dx * dx + dy * dy) > dpad_r(&L) * 0.2f) {
                const float a = atan2f(dy, dx) * 180 / 3.14159265f;     /* 0 = right, 90 = down */
                if (a > -67.5f && a < 67.5f) b |= PSP_CTRL_RIGHT;
                if (a > 22.5f && a < 157.5f) b |= PSP_CTRL_DOWN;
                if (a > 112.5f || a < -112.5f) b |= PSP_CTRL_LEFT;
                if (a > -157.5f && a < -22.5f) b |= PSP_CTRL_UP;
            }
        } else b |= elem_button(f->elem);
    }
    g_touch_buttons = b;
    g_touch_ax = ax;
    g_touch_ay = ay;
    g_touch_rx = rx;
    g_touch_ry = ry;
    g_touch_rheld = rheld;
    SDL_SetAtomicInt(&g_touch_btn_atomic, (int)b);
    SDL_SetAtomicInt(&g_touch_stick_atomic, ax < 0 ? -1 : (ax | ay << 8));
}

static void touch_release_all(void) {
    memset(g_fingers, 0, sizeof g_fingers);
    g_touch_buttons = 0;
    g_touch_ax = g_touch_ay = -1;
    g_touch_rx = g_touch_ry = 0;
    g_touch_rheld = 0;
    SDL_SetAtomicInt(&g_touch_btn_atomic, 0);
    SDL_SetAtomicInt(&g_touch_stick_atomic, -1);
}

/* The colours, at the chosen opacity. */
static uint32_t va(int r, int g, int b, int a) { return rgba(r, g, b, a * g_vpad_alpha / 100); }

static void draw_pill(canvas *c, float s, const layout *L, int g, uint32_t bit, const char *label, uint32_t fillc, uint32_t on, uint32_t line, uint32_t white, float t) {
    float hw, hh;
    pill_size(L, g, &hw, &hh);
    round_rect(c, (L->x[g] - hw) * s, (L->y[g] - hh) * s, 2 * hw * s, 2 * hh * s, hh * s, g_touch_buttons & bit ? on : fillc, t, line);
    text_centered(c, L->x[g] * s, L->y[g] * s, (int)(hh * (g == G_L || g == G_R ? 1.0f : 0.95f) * s), white, label);
}

static void draw_vpad(canvas *c, float s) {
    const layout L = layout_for((float)c->w / s, (float)c->h / s);
    const uint32_t line = va(236, 194, 104, 150), fillc = va(20, 15, 10, 70), on = va(236, 194, 104, 150);
    const uint32_t white = va(250, 245, 235, 190);
    const float t = 2.2f * s;
#define S(v) ((v) * s)
    /* L, R, Select, Start */
    draw_pill(c, s, &L, G_L, PSP_CTRL_LTRIGGER, "L", fillc, on, line, white, t);
    draw_pill(c, s, &L, G_R, PSP_CTRL_RTRIGGER, "R", fillc, on, line, white, t);
    draw_pill(c, s, &L, G_SELECT, PSP_CTRL_SELECT, "SELECT", fillc, on, line, white, t);
    draw_pill(c, s, &L, G_START, PSP_CTRL_START, "START", fillc, on, line, white, t);
    /* the d-pad: four arms */
    const float dr = dpad_r(&L), a = dr * 0.30f, len = dr * 0.80f;
    const struct { float ox, oy; uint32_t bit; } arms[4] = {
        { 0, -1, PSP_CTRL_UP }, { 0, 1, PSP_CTRL_DOWN }, { -1, 0, PSP_CTRL_LEFT }, { 1, 0, PSP_CTRL_RIGHT } };
    for (int i = 0; i < 4; i++) {
        const float cx = L.x[G_DPAD] + arms[i].ox * len, cy = L.y[G_DPAD] + arms[i].oy * len;
        round_rect(c, S(cx - a), S(cy - a), S(2 * a), S(2 * a), S(a * 0.3f), g_touch_buttons & arms[i].bit ? on : fillc, t, line);
        const float tx = arms[i].ox, ty = arms[i].oy, px = -ty, py = tx;
        segment(c, S(cx + tx * a * 0.4f), S(cy + ty * a * 0.4f), S(cx - tx * a * 0.2f + px * a * 0.4f), S(cy - ty * a * 0.2f + py * a * 0.4f), t, white);
        segment(c, S(cx + tx * a * 0.4f), S(cy + ty * a * 0.4f), S(cx - tx * a * 0.2f - px * a * 0.4f), S(cy - ty * a * 0.2f - py * a * 0.4f), t, white);
    }
    /* the stick */
    const float sr = stick_r(&L);
    disc(c, S(L.x[G_STICK]), S(L.y[G_STICK]), S(sr), fillc);
    ring(c, S(L.x[G_STICK]), S(L.y[G_STICK]), S(sr), t, line);
    float kx = L.x[G_STICK], ky = L.y[G_STICK];
    if (g_touch_ax >= 0) { kx += (g_touch_ax - 128) / 127.0f * sr; ky += (g_touch_ay - 128) / 127.0f * sr; }
    disc(c, S(kx), S(ky), S(sr * 0.45f), g_touch_ax >= 0 ? on : va(236, 194, 104, 90));
    ring(c, S(kx), S(ky), S(sr * 0.45f), t, line);
    /* the right stick: the camera (the d-pad) */
    const float rr = rstick_r(&L);
    disc(c, S(L.x[G_RSTICK]), S(L.y[G_RSTICK]), S(rr), fillc);
    ring(c, S(L.x[G_RSTICK]), S(L.y[G_RSTICK]), S(rr), t, line);
    const float rkx = L.x[G_RSTICK] + g_touch_rx * rr, rky = L.y[G_RSTICK] + g_touch_ry * rr;
    disc(c, S(rkx), S(rky), S(rr * 0.45f), g_touch_rheld ? on : va(236, 194, 104, 90));
    ring(c, S(rkx), S(rky), S(rr * 0.45f), t, line);
    if (!g_touch_rheld) text_centered(c, S(L.x[G_RSTICK]), S(L.y[G_RSTICK]), (int)S(rr * 0.30f), white, "CAM");
    /* the face buttons, in the PSP's shapes and colours */
    const struct { float ox, oy; uint32_t bit; int shape; uint32_t tint; } face[4] = {
        { 0, 1, PSP_CTRL_CROSS, 0, va(140, 180, 245, 220) }, { 1, 0, PSP_CTRL_CIRCLE, 1, va(245, 120, 120, 220) },
        { -1, 0, PSP_CTRL_SQUARE, 2, va(245, 155, 215, 220) }, { 0, -1, PSP_CTRL_TRIANGLE, 3, va(120, 220, 180, 220) } };
    const float fo = face_o(&L), r = face_r(&L), k = r * 0.42f;
    for (int i = 0; i < 4; i++) {
        const float cx = L.x[G_FACE] + face[i].ox * fo, cy = L.y[G_FACE] + face[i].oy * fo;
        const int pressed = (g_touch_buttons & face[i].bit) != 0;
        disc(c, S(cx), S(cy), S(r), pressed ? on : fillc);
        ring(c, S(cx), S(cy), S(r), t, line);
        const uint32_t sc = face[i].tint;
        if (face[i].shape == 0) { segment(c, S(cx - k), S(cy - k), S(cx + k), S(cy + k), t * 1.4f, sc); segment(c, S(cx - k), S(cy + k), S(cx + k), S(cy - k), t * 1.4f, sc); }
        else if (face[i].shape == 1) ring(c, S(cx), S(cy), S(k), t * 1.4f, sc);
        else if (face[i].shape == 2) {
            segment(c, S(cx - k), S(cy - k), S(cx + k), S(cy - k), t * 1.4f, sc); segment(c, S(cx + k), S(cy - k), S(cx + k), S(cy + k), t * 1.4f, sc);
            segment(c, S(cx + k), S(cy + k), S(cx - k), S(cy + k), t * 1.4f, sc); segment(c, S(cx - k), S(cy + k), S(cx - k), S(cy - k), t * 1.4f, sc);
        } else {
            segment(c, S(cx), S(cy - k * 1.1f), S(cx + k), S(cy + k * 0.7f), t * 1.4f, sc); segment(c, S(cx + k), S(cy + k * 0.7f), S(cx - k), S(cy + k * 0.7f), t * 1.4f, sc);
            segment(c, S(cx - k), S(cy + k * 0.7f), S(cx), S(cy - k * 1.1f), t * 1.4f, sc);
        }
    }
#undef S
}

/* The menu button: three bars in a gold ring. Drawn whenever the touch
 * controls could be wanted (on a phone), even with them hidden. */
static void draw_menu_button(canvas *c, float s) {
    const layout L = layout_for((float)c->w / s, (float)c->h / s);
    const float x = L.x[G_MENU] * s, y = L.y[G_MENU] * s, r = menu_r(&L) * s;
    disc(c, x, y, r, rgba(20, 15, 10, 120));
    ring(c, x, y, r, 2.2f * s, rgba(236, 194, 104, 170));
    for (int i = -1; i <= 1; i++) segment(c, x - r * 0.45f, y + i * r * 0.32f, x + r * 0.45f, y + i * r * 0.32f, 2.4f * s, rgba(250, 245, 235, 210));
}

/* ---- the layout editor ------------------------------------------------------------------
 *
 * Every group is drawn with its bounds; touch one to pick it and drag it
 * anywhere. The toolbar at the top makes the picked one smaller or bigger,
 * resets everything to the defaults, or saves and closes (Done, or Back). */

enum { E_SMALLER, E_BIGGER, E_RESET, E_DONE, E_COUNT };
static const char *const k_edit_label[E_COUNT] = { "Smaller", "Bigger", "Reset", "Done" };
static float g_drag_dx, g_drag_dy;
static SDL_FingerID g_drag_finger;
static int g_dragging;

static void edit_toolbar(float w, float h, float *x, float *y, float *bw, float *bh, float *gap) {
    *bh = h * 0.085f;
    *bw = h * 0.26f;
    *gap = h * 0.025f;
    *x = (w - (*bw * E_COUNT + *gap * (E_COUNT - 1))) / 2;
    *y = h * 0.035f;
}

static void edit_open(void) {
    g_edit = 1;
    g_edit_sel = -1;
    g_dragging = 0;
    touch_release_all();
}

static void edit_close(void) {
    g_edit = 0;
    g_dragging = 0;
    settings_save();
}

/* A group moved to a centre (cx, cy) in window pixels, kept on screen. */
static void edit_place(const layout *L, int g, float cx, float cy) {
    float x0, y0, x1, y1;
    group_box(L, g, &x0, &y0, &x1, &y1);
    const float rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    cx = fminf(fmaxf(cx, rx), L->w - rx);
    cy = fminf(fmaxf(cy, ry), L->h - ry);
    g_place[g] = (placed){ 1, cx / L->w, cy / L->h, L->k[g] };
}

static void edit_resize(const layout *L, int g, float factor) {
    float k = L->k[g] * factor;
    k = fminf(fmaxf(k, 0.5f), 2.0f);
    g_place[g] = (placed){ 1, L->x[g] / L->w, L->y[g] / L->h, k };
}

static void edit_touch(const SDL_Event *e, float w, float h) {
    const float px = e->tfinger.x * w, py = e->tfinger.y * h;
    const layout L = layout_for(w, h);
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        float tx, ty, bw, bh, gap;
        edit_toolbar(w, h, &tx, &ty, &bw, &bh, &gap);
        if (py >= ty && py < ty + bh)
            for (int i = 0; i < E_COUNT; i++) {
                const float bx = tx + i * (bw + gap);
                if (px < bx || px >= bx + bw) continue;
                if (i == E_DONE) edit_close();
                else if (i == E_RESET) { memset(g_place, 0, sizeof g_place); g_edit_sel = -1; }
                else if (g_edit_sel >= 0) edit_resize(&L, g_edit_sel, i == E_BIGGER ? 1.1f : 1 / 1.1f);
                return;
            }
        /* The group under the finger, the smallest first (so the stick does
         * not swallow the d-pad above it). */
        int best = -1;
        float best_area = 1e30f;
        for (int g = 0; g < G_COUNT; g++) {
            float x0, y0, x1, y1;
            group_box(&L, g, &x0, &y0, &x1, &y1);
            if (px >= x0 && px < x1 && py >= y0 && py < y1 && (x1 - x0) * (y1 - y0) < best_area) {
                best = g;
                best_area = (x1 - x0) * (y1 - y0);
            }
        }
        g_edit_sel = best;
        if (best >= 0) {
            g_dragging = 1;
            g_drag_finger = e->tfinger.fingerID;
            g_drag_dx = L.x[best] - px;
            g_drag_dy = L.y[best] - py;
        }
    } else if (e->type == SDL_EVENT_FINGER_MOTION && g_dragging && e->tfinger.fingerID == g_drag_finger && g_edit_sel >= 0) {
        edit_place(&L, g_edit_sel, px + g_drag_dx, py + g_drag_dy);
    } else if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) && e->tfinger.fingerID == g_drag_finger) {
        g_dragging = 0;
    }
}

static void draw_editor(canvas *c, float s) {
    const float w = c->w / s, h = c->h / s;
    fill(c, 0, 0, c->w, c->h, rgba(0, 0, 0, 90));
    draw_vpad(c, s);
    draw_menu_button(c, s);
    const layout L = layout_for(w, h);
    const uint32_t gold = rgba(236, 194, 104, 255);
    for (int g = 0; g < G_COUNT; g++) {
        float x0, y0, x1, y1;
        group_box(&L, g, &x0, &y0, &x1, &y1);
        const int sel = g == g_edit_sel;
        frame_rect(c, (int)(x0 * s), (int)(y0 * s), (int)((x1 - x0) * s), (int)((y1 - y0) * s), sel ? 2 : 1,
                   sel ? gold : rgba(236, 194, 104, 110));
        if (sel) {
            diamond(c, (int)(x0 * s), (int)(y0 * s), (int)(5 * s), gold);
            diamond(c, (int)(x1 * s), (int)(y0 * s), (int)(5 * s), gold);
            diamond(c, (int)(x0 * s), (int)(y1 * s), (int)(5 * s), gold);
            diamond(c, (int)(x1 * s), (int)(y1 * s), (int)(5 * s), gold);
        }
    }
    float tx, ty, bw, bh, gap;
    edit_toolbar(w, h, &tx, &ty, &bw, &bh, &gap);
    for (int i = 0; i < E_COUNT; i++) {
        const float bx = tx + i * (bw + gap);
        const int dim = (i == E_SMALLER || i == E_BIGGER) && g_edit_sel < 0;
        round_rect(c, bx * s, ty * s, bw * s, bh * s, bh / 2 * s, i == E_DONE ? rgba(150, 60, 25, 235) : rgba(30, 23, 17, 235),
                   2 * s, dim ? rgba(126, 98, 54, 255) : gold);
        text_centered(c, (bx + bw / 2) * s, (ty + bh / 2) * s, (int)(bh * 0.40f * s),
                      dim ? rgba(150, 140, 120, 255) : rgba(252, 247, 237, 255), k_edit_label[i]);
    }
    text_centered(c, w / 2 * s, (ty + bh + h * 0.05f) * s, (int)(h * 0.030f * s), rgba(252, 247, 237, 230),
                  g_edit_sel >= 0 ? "Drag to move it; Smaller and Bigger resize it" : "Touch a control to move or resize it");
}

/* ---- the in-game menu: a drawer from the left ------------------------------------------
 *
 * As in Bloodborne-Android: swipe in from the left edge, press Back (or F1),
 * or tap the menu button; the panel slides over the left of the game, the
 * rest dims, and a tap outside closes it. Rows are grouped (Game, Online,
 * Controls, App), the setting's name on the left and its value on the right;
 * when they do not fit, a drag scrolls them. Online and the update check are
 * Android dialogs (MhfuActivity, over JNI): they need text fields. */

enum { R_HEAD, R_ITEM };
enum { M_RESUME, M_NET, M_ACH, M_VPAD, M_EDIT, M_OPACITY, M_FPS, M_UPDATE, M_EXIT };
typedef struct { int kind, id; const char *text; } menu_row;
static const menu_row k_menu[] = {
    { R_HEAD, -1, "GAME" },     { R_ITEM, M_RESUME, "Back to game" },
#ifdef __ANDROID__
    { R_HEAD, -1, "ONLINE" },   { R_ITEM, M_NET, "Multiplayer" }, { R_ITEM, M_ACH, "RetroAchievements" },
#endif
    { R_HEAD, -1, "CONTROLS" }, { R_ITEM, M_VPAD, "Virtual gamepad" }, { R_ITEM, M_EDIT, "Edit button layout" },
    { R_ITEM, M_OPACITY, "Button opacity" }, { R_ITEM, M_FPS, "Show FPS" },
    { R_HEAD, -1, "APP" },
#ifdef __ANDROID__
    { R_ITEM, M_UPDATE, "Check for updates" },
#endif
    { R_ITEM, M_EXIT, "Exit game" },
};
#define MENU_ROWS (int)(sizeof k_menu / sizeof k_menu[0])
#define MENU_MS   220

static uint64_t g_menu_t0;                 /* when it last started opening or closing */
static float    g_menu_scroll;             /* how far the rows are scrolled up, window pixels */
static int      g_mdrag_on, g_mdragged;    /* a finger on the drawer; it moved enough to scroll */
static SDL_FingerID g_mdrag_id;
static float    g_mdown_x, g_mdown_y, g_mdown_scroll;

#ifdef __ANDROID__
#include <jni.h>
/* A no-argument method of MhfuActivity: checkUpdates, openMultiplayer,
 * openAchievements -- dialogs over the game. */
static void android_call(const char *method) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, method, "()V");
    if (m) (*env)->CallVoidMethod(env, act, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif

static void menu_set(int open) {
    if (open == g_menu_open) return;
    /* Start from wherever the slide is now. */
    const uint64_t now = SDL_GetTicks(), done = now - g_menu_t0 >= MENU_MS ? MENU_MS : now - g_menu_t0;
    g_menu_t0 = now - (MENU_MS - done);
    g_menu_open = open;
    if (open) touch_release_all();
}

/* How far the drawer is out, 0..1; *moving while it slides. */
static float menu_progress(int *moving) {
    const uint64_t t = SDL_GetTicks() - g_menu_t0;
    float f = t >= MENU_MS ? 1.0f : (float)t / MENU_MS;
    if (moving) *moving = t < MENU_MS;
    f = 1 - (1 - f) * (1 - f) * (1 - f);
    return g_menu_open ? f : 1 - f;
}

static const char *menu_value(int id) {
    static const char *const vp[3] = { "Automatic", "On", "Off" };
    switch (id) {
    case M_VPAD: return vp[g_vpad_mode];
    case M_FPS: return g_show_fps ? "On" : "Off";
    case M_NET: {
        static char code[8];
        mhfu_lobby_code(code);
        return code[0] ? code : "Offline";
    }
    case M_ACH: {
        const char *u = mhfu_ach_user();
        return u[0] ? u : "Off";
    }
    case M_OPACITY: {
        static char pct[8];
        snprintf(pct, sizeof pct, "%d%%", g_vpad_alpha);
        return pct;
    }
    default: return "";
    }
}

/* The drawer's width and its rows' heights, in window pixels. */
static void menu_metrics(float w, float h, float *pw, float *head, float *row, float *top) {
    *pw = fmaxf(fminf(w * 0.40f, h * 1.0f), h * 0.75f);
    *head = h * 0.065f;
    *row = h * 0.088f;
    *top = h * 0.19f;
}

/* Where the rows end (the hint line is below), and how far they can scroll. */
static float menu_bottom(float h) { return h * 0.92f; }
static float menu_max_scroll(float h) {
    float pw, head, row, top, all = 0;
    menu_metrics(h, h, &pw, &head, &row, &top);
    for (int i = 0; i < MENU_ROWS; i++) all += k_menu[i].kind == R_HEAD ? head : row;
    return fmaxf(0, all - (menu_bottom(h) - top));
}

static void draw_menu(canvas *c, float s, float prog) {
    const float w = c->w / s, h = c->h / s;
    float pw, head, row, top;
    menu_metrics(w, h, &pw, &head, &row, &top);
    const float x0 = -(1 - prog) * pw;                    /* slides in from the left */
    fill(c, 0, 0, c->w, c->h, rgba(0, 0, 0, (int)(140 * prog)));
    const uint32_t gold = rgba(236, 194, 104, 255), brass = rgba(126, 98, 54, 255), wood = rgba(26, 20, 15, 248);
    fill(c, (int)(x0 * s), 0, (int)(pw * s), c->h, wood);
    /* a brass edge, double, with a stud at the middle */
    fill(c, (int)((x0 + pw) * s) - (int)(2.5f * s), 0, (int)(2.5f * s), c->h, gold);
    fill(c, (int)((x0 + pw) * s) - (int)(8 * s), 0, (int)(1 * s) + 1, c->h, brass);
    diamond(c, (int)((x0 + pw) * s) - (int)(1 * s), c->h / 2, (int)(7 * s), gold);
    /* the crest */
    const float cx = x0 + pw * 0.08f;
    text(c, (int)(cx * s), (int)(h * 0.085f * s), (int)(h * 0.030f * s), rgba(205, 192, 168, 255), "MONSTER HUNTER", 0);
    text(c, (int)(cx * s), (int)(h * 0.145f * s), (int)(h * 0.052f * s), gold, "FREEDOM UNITE", 0);
    segment(c, cx * s, h * 0.175f * s, (x0 + pw * 0.92f) * s, h * 0.175f * s, 1.2f * s, brass);
    const float maxs = menu_max_scroll(h);
    g_menu_scroll = fminf(fmaxf(g_menu_scroll, 0), maxs);
    float y = top - g_menu_scroll;
    g_clip0 = (int)(top * s);
    g_clip1 = (int)(menu_bottom(h) * s);
    if (maxs > 0) {                                       /* where in the list: a brass bar */
        const float span = menu_bottom(h) - top, bar = span * span / (span + maxs);
        const float by = top + (span - bar) * g_menu_scroll / maxs;
        fill(c, (int)((x0 + pw * 0.965f) * s), (int)(by * s), (int)(3 * s) + 1, (int)(bar * s), rgba(126, 98, 54, 200));
    }
    for (int i = 0; i < MENU_ROWS; i++) {
        const menu_row *r = &k_menu[i];
        if (r->kind == R_HEAD) {
            text(c, (int)(cx * s), (int)((y + head * 0.72f) * s), (int)(h * 0.028f * s), gold, r->text, 0);
            y += head;
            continue;
        }
        const int px = (int)(h * 0.040f * s);
        const uint32_t col = r->id == M_EXIT ? rgba(236, 140, 120, 255) : rgba(252, 247, 237, 255);
        diamond(c, (int)((cx + h * 0.012f) * s), (int)((y + row / 2) * s), (int)(h * 0.009f * s), brass);
        text(c, (int)((cx + h * 0.04f) * s), (int)((y + row / 2 + h * 0.014f) * s), px, col, r->text, 0);
        const char *v = menu_value(r->id);
        if (*v) {
            const int vw = text_width(v, px);
            text(c, (int)((x0 + pw * 0.92f) * s) - vw, (int)((y + row / 2 + h * 0.014f) * s), px, gold, v, 0);
        }
        segment(c, cx * s, (y + row) * s, (x0 + pw * 0.92f) * s, (y + row) * s, 1.0f * s, rgba(126, 98, 54, 140));
        y += row;
    }
    g_clip0 = 0;
    g_clip1 = 1 << 30;
    text(c, (int)(cx * s), (int)((h - h * 0.04f) * s), (int)(h * 0.026f * s), rgba(160, 148, 128, 255),
         "Swipe from the left edge or press Back to open", 0);
}

/* A tap with the drawer open. */
static void menu_tap(float px, float py, float w, float h) {
    float pw, head, row, top;
    menu_metrics(w, h, &pw, &head, &row, &top);
    if (px > pw) { menu_set(0); return; }                /* outside: close */
    if (py < top || py >= menu_bottom(h)) return;
    float y = top - g_menu_scroll;
    for (int i = 0; i < MENU_ROWS; i++) {
        const menu_row *r = &k_menu[i];
        const float rh = r->kind == R_HEAD ? head : row;
        if (r->kind == R_ITEM && py >= y && py < y + rh) {
            switch (r->id) {
            case M_RESUME: menu_set(0); break;
            case M_VPAD: g_vpad_mode = (g_vpad_mode + 1) % 3; settings_save(); break;
            case M_FPS: g_show_fps = !g_show_fps; g_show_fps_init = g_show_fps; settings_save(); break;
            case M_EDIT: menu_set(0); edit_open(); break;
#ifdef __ANDROID__
            case M_UPDATE: menu_set(0); android_call("checkUpdates"); break;
            case M_NET: menu_set(0); android_call("openMultiplayer"); break;
            case M_ACH: menu_set(0); android_call("openAchievements"); break;
#endif
            case M_OPACITY: g_vpad_alpha = g_vpad_alpha >= 100 ? 25 : g_vpad_alpha + 25; settings_save(); break;
            case M_EXIT: {
                SDL_Event e;
                SDL_zero(e);
                e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
                SDL_PushEvent(&e);
                break;
            }
            }
            return;
        }
        y += rh;
    }
}

/* A finger with the drawer open: a tap picks a row, a drag scrolls them. */
static void menu_touch(const SDL_Event *e, float px, float py, float w, float h) {
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
        if (g_mdrag_on) return;
        g_mdrag_on = 1;
        g_mdrag_id = e->tfinger.fingerID;
        g_mdragged = 0;
        g_mdown_x = px; g_mdown_y = py; g_mdown_scroll = g_menu_scroll;
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (!g_mdrag_on || e->tfinger.fingerID != g_mdrag_id) return;
        if (fabsf(py - g_mdown_y) > h * 0.025f) g_mdragged = 1;
        if (g_mdragged) g_menu_scroll = fminf(fmaxf(g_mdown_scroll - (py - g_mdown_y), 0), menu_max_scroll(h));
        break;
    case SDL_EVENT_FINGER_UP:
        if (!g_mdrag_on || e->tfinger.fingerID != g_mdrag_id) return;
        g_mdrag_on = 0;
        if (!g_mdragged) menu_tap(g_mdown_x, g_mdown_y, w, h);
        break;
    default:
        g_mdrag_on = 0;
        break;
    }
}

/* ---- toasts ------------------------------------------------------------------------ */

#define TOAST_MAX   8
#define TOAST_IN_MS 320
#define TOAST_ON_MS 5200

typedef struct { char head[64], title[256], text[512], badge[700]; } toast;
static toast    g_toasts[TOAST_MAX];
static int      g_ntoasts;
static uint64_t g_toast_t0;
static unsigned char *g_badge;
static int      g_badge_w, g_badge_h, g_badge_loaded;

/* How far the showing toast has slid in (0..1): 1 while one shows, 0 for
 * none, -1 when it is done. Under g_lock. */
static int toast_phase(uint64_t now, float *in) {
    if (!g_ntoasts || !g_toast_t0) return 0;
    const uint64_t t = now - g_toast_t0;
    float f;
    if (t < TOAST_IN_MS) f = (float)t / TOAST_IN_MS;
    else if (t < TOAST_IN_MS + TOAST_ON_MS) f = 1;
    else if (t < TOAST_IN_MS * 2 + TOAST_ON_MS) f = 1 - (float)(t - TOAST_IN_MS - TOAST_ON_MS) / TOAST_IN_MS;
    else return -1;
    *in = 1 - (1 - f) * (1 - f) * (1 - f);
    return 1;
}

static void draw_toast(canvas *c, int x, int y, int w, int h, const toast *t) {
    const float u = g_ui;
    const uint32_t gold = rgba(236, 194, 104, 255), brass = rgba(126, 98, 54, 255);
    fill(c, x, y, w, h, rgba(30, 23, 17, 242));
    frame_rect(c, x, y, w, h, (int)(2 * u), gold);
    frame_rect(c, x + (int)(5 * u), y + (int)(5 * u), w - (int)(10 * u), h - (int)(10 * u), 1, brass);
    const int bs = (int)(64 * u), bx = x + (int)(20 * u), by = y + (h - bs) / 2;
    if (g_badge) {
        for (int j = 0; j < bs; j++)
            for (int i = 0; i < bs; i++) {
                const unsigned char *p = &g_badge[((j * g_badge_h / bs) * g_badge_w + i * g_badge_w / bs) * 4];
                blend(c, bx + i, by + j, rgba(p[0], p[1], p[2], 255), p[3]);
            }
    } else diamond(c, bx + bs / 2, by + bs / 2, bs / 2 - (int)(6 * u), gold);
    frame_rect(c, bx - 2, by - 2, bs + 4, bs + 4, 2, gold);
    const int tx = bx + bs + (int)(18 * u), tw = x + w - (int)(16 * u) - tx;
    text(c, tx, y + (int)(29 * u), (int)(13 * u), gold, t->head, tw);
    text(c, tx, y + (int)(53 * u), (int)(20 * u), rgba(252, 247, 237, 255), t->title, tw);
    text_wrapped(c, tx, y + (int)(76 * u), (int)(15 * u), (int)(18 * u), rgba(205, 192, 168, 255), t->text, tw, 2);
}

/* ---- the text box: the game's keyboard ---------------------------------------------- */

#define TEXT_MAX 128
static char          g_tb_prompt[128], g_tb_text[TEXT_MAX * 4 + 1];
static int           g_tb_limit;
static SDL_AtomicInt g_tb_state;                  /* 0 none, 1 open, 2 done, 3 cancelled */

static int utf8_len(const char *s) { int n = 0; while (*s) { utf8_next(&s); n++; } return n; }

static void draw_textbox(canvas *c) {
    const float u = g_ui;
    const int w = (int)(560 * u) < c->w - 40 ? (int)(560 * u) : c->w - 40, h = (int)(150 * u);
    const int x = (c->w - w) / 2, y = (int)(60 * u);
    const uint32_t gold = rgba(236, 194, 104, 255);
    fill(c, x, y, w, h, rgba(30, 23, 17, 245));
    frame_rect(c, x, y, w, h, (int)(2 * u), gold);
    text(c, x + (int)(22 * u), y + (int)(36 * u), (int)(16 * u), gold, g_tb_prompt, w - (int)(44 * u));
    fill(c, x + (int)(20 * u), y + (int)(52 * u), w - (int)(40 * u), (int)(44 * u), rgba(12, 9, 7, 255));
    frame_rect(c, x + (int)(20 * u), y + (int)(52 * u), w - (int)(40 * u), (int)(44 * u), 1, rgba(126, 98, 54, 255));
    char shown[sizeof g_tb_text + 2];
    snprintf(shown, sizeof shown, "%s%s", g_tb_text, (SDL_GetTicks() / 500) % 2 ? "|" : "");
    text(c, x + (int)(32 * u), y + (int)(82 * u), (int)(20 * u), rgba(252, 247, 237, 255), shown, w - (int)(64 * u));
    text(c, x + (int)(22 * u), y + (int)(128 * u), (int)(14 * u), rgba(205, 192, 168, 255),
         "Enter: OK    Escape: cancel", w - (int)(44 * u));
}

/* ---- composing the overlay (on the loop's thread) --------------------------------------- */

static char g_fps_text[96] = "FPS --";

static int compose(void) {
    if (!g_gl) return 0;
    SDL_LockMutex(g_lock);
    float in = 0;
    int showing = toast_phase(SDL_GetTicks(), &in);
    while (showing < 0) {
        memmove(g_toasts, g_toasts + 1, (size_t)(--g_ntoasts) * sizeof *g_toasts);
        stbi_image_free(g_badge);
        g_badge = NULL;
        g_badge_loaded = 0;
        g_toast_t0 = g_ntoasts ? SDL_GetTicks() : 0;
        showing = toast_phase(SDL_GetTicks(), &in);
    }
    if (showing && !g_badge_loaded) {
        g_badge_loaded = 1;
        int n;
        if (g_toasts[0].badge[0]) g_badge = stbi_load(g_toasts[0].badge, &g_badge_w, &g_badge_h, &n, 4);
    }
    const int box = SDL_GetAtomicInt(&g_tb_state) == 1;
    const int fps = g_show_fps && g_font_ok;
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
#ifdef __ANDROID__
    const int layer = 1;                         /* the menu button is always there on a phone */
#else
    const int layer = g_edit || g_menu_open || vpad_visible() || menu_progress(NULL) > 0;
#endif
    if (layer && ww > 0 && wh > 0) {
        /* The whole screen at half size, scaled up when drawn: the touch
         * controls, the menu button, the menu, and on top the FPS line,
         * the toast and the text box drawn at half size too. */
        const float s = 0.5f;
        canvas c = { NULL, (int)(ww * s), (int)(wh * s) };
        c.px = (uint32_t *)calloc((size_t)c.w * c.h, 4);
        if (!c.px) { SDL_UnlockMutex(g_lock); return 0; }
        const float ui = g_ui;
        g_ui *= s;
        int sliding = 0;
        const float prog = menu_progress(&sliding);
        if (g_edit) draw_editor(&c, s);
        else {
            if (vpad_visible() && !g_menu_open) draw_vpad(&c, s);
            if (!g_menu_open) draw_menu_button(&c, s);
        }
        if (fps) {
            const int fpx = (int)(14 * g_ui), fw = text_width(g_fps_text, fpx) + (int)(10 * g_ui), fh = fpx + (int)(8 * g_ui);
            fill(&c, 0, 0, fw, fh, rgba(0, 0, 0, 192));
            text(&c, (int)(5 * g_ui), fpx + (int)(2 * g_ui), fpx, rgba(80, 255, 80, 255), g_fps_text, 0);
        }
        if (showing) {
            const int tw = (int)(440 * g_ui), th = (int)(116 * g_ui);
            draw_toast(&c, c.w - tw - (int)(20 * g_ui) + (int)((1 - in) * (tw + 30 * g_ui)), (int)(20 * g_ui), tw, th, &g_toasts[0]);
        }
        if (prog > 0) draw_menu(&c, s, prog);
        if (box) draw_textbox(&c);
        g_ui = ui;
        SDL_UnlockMutex(g_lock);
        psp_render_gl_overlay_scaled(c.px, c.w, c.h, ww, wh);
        free(c.px);
        return showing || box || sliding;
    }
    const int fpx = (int)(14 * g_ui), fw = fps ? text_width(g_fps_text, fpx) + (int)(10 * g_ui) : 0, fh = fps ? fpx + (int)(8 * g_ui) : 0;
    const int tw = (int)(440 * g_ui), th = (int)(116 * g_ui);
    canvas c = { NULL, fw, fh };
    if (showing) { c.w = ww; c.h = th + (int)(24 * g_ui) > c.h ? th + (int)(24 * g_ui) : c.h; }
    if (box) { c.w = ww; c.h = (int)(230 * g_ui) > c.h ? (int)(230 * g_ui) : c.h; }
    if (c.w <= 0 || c.h <= 0) { SDL_UnlockMutex(g_lock); psp_render_gl_overlay(NULL, 0, 0); return 0; }
    c.px = (uint32_t *)calloc((size_t)c.w * c.h, 4);
    if (!c.px) { SDL_UnlockMutex(g_lock); return 0; }
    if (fps) {
        fill(&c, 0, 0, fw, fh, rgba(0, 0, 0, 192));
        text(&c, (int)(5 * g_ui), fpx + (int)(2 * g_ui), fpx, rgba(80, 255, 80, 255), g_fps_text, 0);
    }
    if (showing) draw_toast(&c, c.w - tw - (int)(20 * g_ui) + (int)((1 - in) * (tw + 30 * g_ui)), (int)(20 * g_ui), tw, th, &g_toasts[0]);
    if (box) draw_textbox(&c);
    SDL_UnlockMutex(g_lock);
    psp_render_gl_overlay(c.px, c.w, c.h);
    free(c.px);
    return showing || box;                       /* still moving: compose again soon */
}

/* ---- keys ---------------------------------------------------------------------------- */

/* input.ini's names, by SDL scancode (the launcher's keys.cpp has the same). */
static const char *key_name(SDL_Scancode sc) {
    static char one[2];
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { one[0] = (char)('a' + (sc - SDL_SCANCODE_A)); return one; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { one[0] = (char)('1' + (sc - SDL_SCANCODE_1)); return one; }
    static char f[8];
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) { snprintf(f, sizeof f, "f%d", 1 + (sc - SDL_SCANCODE_F1)); return f; }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) { snprintf(f, sizeof f, "kp%d", 1 + (sc - SDL_SCANCODE_KP_1)); return f; }
    switch (sc) {
    case SDL_SCANCODE_0: return "0";
    case SDL_SCANCODE_KP_0: return "kp0";
    case SDL_SCANCODE_SPACE: return "space";
    case SDL_SCANCODE_RETURN: return "enter";
    case SDL_SCANCODE_BACKSPACE: return "backspace";
    case SDL_SCANCODE_TAB: return "tab";
    case SDL_SCANCODE_ESCAPE: return "escape";
    case SDL_SCANCODE_CAPSLOCK: return "capslock";
    case SDL_SCANCODE_LSHIFT: return "lshift";
    case SDL_SCANCODE_RSHIFT: return "rshift";
    case SDL_SCANCODE_LCTRL: return "lctrl";
    case SDL_SCANCODE_RCTRL: return "rctrl";
    case SDL_SCANCODE_LALT: return "lalt";
    case SDL_SCANCODE_RALT: return "ralt";
    case SDL_SCANCODE_UP: return "up";
    case SDL_SCANCODE_DOWN: return "down";
    case SDL_SCANCODE_LEFT: return "left";
    case SDL_SCANCODE_RIGHT: return "right";
    case SDL_SCANCODE_INSERT: return "insert";
    case SDL_SCANCODE_DELETE: return "delete";
    case SDL_SCANCODE_HOME: return "home";
    case SDL_SCANCODE_END: return "end";
    case SDL_SCANCODE_PAGEUP: return "pageup";
    case SDL_SCANCODE_PAGEDOWN: return "pagedown";
    case SDL_SCANCODE_COMMA: return "comma";
    case SDL_SCANCODE_PERIOD: return "period";
    case SDL_SCANCODE_SLASH: return "slash";
    case SDL_SCANCODE_SEMICOLON: return "semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "quote";
    case SDL_SCANCODE_MINUS: return "minus";
    case SDL_SCANCODE_EQUALS: return "equals";
    case SDL_SCANCODE_LEFTBRACKET: return "lbracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "rbracket";
    case SDL_SCANCODE_BACKSLASH: return "backslash";
    case SDL_SCANCODE_GRAVE: return "grave";
    case SDL_SCANCODE_KP_PLUS: return "kp_plus";
    case SDL_SCANCODE_KP_MINUS: return "kp_minus";
    case SDL_SCANCODE_KP_MULTIPLY: return "kp_multiply";
    case SDL_SCANCODE_KP_DIVIDE: return "kp_divide";
    case SDL_SCANCODE_KP_PERIOD: return "kp_period";
    default: return NULL;
    }
}

static unsigned char g_held[SDL_SCANCODE_COUNT];

static void keys_changed(void) {
    uint32_t b = 0;
    int st = 0;
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        if (!g_held[sc]) continue;
        const char *n = key_name((SDL_Scancode)sc);
        const int vk = n ? mhfu_keys_vk(n) : -1;
        if (vk < 0) continue;
        b |= mhfu_keys_buttons((unsigned)vk);
        st |= mhfu_keys_stick((unsigned)vk);
    }
    SDL_SetAtomicInt(&g_buttons, (int)b);
    SDL_SetAtomicInt(&g_stick, st);
}

/* ---- GL, on psprecomp's render thread ---------------------------------------------------- */

static int gl_init(void *user) {
    (void)user;
    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx) { fprintf(stderr, "window: no GL context: %s\n", SDL_GetError()); return -1; }
    SDL_GL_MakeCurrent(g_window, g_ctx);
    typedef const unsigned char *(*get_string_fn)(unsigned);
    get_string_fn get_string = (get_string_fn)(void *)SDL_GL_GetProcAddress("glGetString");
    if (get_string)
        printf("window: %s context: %s on %s\n", g_gles ? "GLES" : "GL", (const char *)get_string(0x1F02 /* GL_VERSION */),
               (const char *)get_string(0x1F01 /* GL_RENDERER */));
    /* No vsync: the game paces itself, and a blocked swap would hold up the
     * frame after it. */
    SDL_GL_SetSwapInterval(0);
    return 0;
}
static void *gl_proc(void *user, const char *name) { (void)user; return (void *)SDL_GL_GetProcAddress(name); }
static void gl_swap(void *user) {
    (void)user;
    /* Android replaces the window's surface when the screen turns or the app
     * comes back; the context has to be made current on it again, here on the
     * render thread (SDL would do it on the thread that pumps events). */
    /* It is tried again every frame until it works: the event that says the
     * app is back can come before Android has the new surface ready (coming
     * back from the setup screen or the home screen left the game running
     * into a surface that was gone: a black screen). */
    if (SDL_GetAtomicInt(&g_remake) && SDL_GL_MakeCurrent(g_window, g_ctx)) SDL_SetAtomicInt(&g_remake, 0);
    if (!SDL_GL_SwapWindow(g_window)) SDL_SetAtomicInt(&g_remake, 1);
}
static void gl_size(void *user, int *w, int *h) { (void)user; SDL_GetWindowSizeInPixels(g_window, w, h); }

/* ---- the API the game calls ------------------------------------------------------------ */

void mhfu_window_options(int fullscreen, int out_w, int out_h, const char *keys_ini) {
    g_fullscreen = fullscreen;
    g_out_w = out_w;
    g_out_h = out_h;
    mhfu_keys_load(keys_ini);
}

static void wake(Uint32 type) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    SDL_PushEvent(&e);
}

int mhfu_window_start(int scale) {
    g_scale = scale > 0 ? scale : 2;
    SDL_LockMutex(g_lock);
    g_window_state = 0;
    wake(g_ev_window);
    while (g_window_state == 0) SDL_WaitCondition(g_cond, g_lock);
    const int ok = g_window_state > 0;
    SDL_UnlockMutex(g_lock);
    return ok ? 0 : -1;
}

int mhfu_window_gl(void) {
    static psp_gl_host host;
    memset(&host, 0, sizeof host);
    host.gles = g_gles;
    host.init = gl_init;
    host.get_proc = gl_proc;
    host.swap = gl_swap;
    host.size = gl_size;
    if (psp_render_gl_start(&host, g_scale) != 0) return -1;
    if (psp_render_select("gl") != 0) return -1;
    g_gl = 1;
    wake(g_ev_wake);
    return 0;
}

int mhfu_window_closed(void) { return SDL_GetAtomicInt(&g_closed); }
void mhfu_window_on_close(void (*fn)(void)) { g_on_close = fn; }

uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) {
    const int s = SDL_GetAtomicInt(&g_stick);
    *ax = (s & 1) ? 0 : (s & 2) ? 255 : 128;
    *ay = (s & 4) ? 0 : (s & 8) ? 255 : 128;
    const int t = SDL_GetAtomicInt(&g_touch_stick_atomic);
    if (t >= 0) { *ax = (uint8_t)(t & 0xFF); *ay = (uint8_t)(t >> 8); }
    return (uint32_t)SDL_GetAtomicInt(&g_buttons) | (uint32_t)SDL_GetAtomicInt(&g_touch_btn_atomic);
}

int mhfu_window_ask_text(const char *prompt, const uint16_t *initial, int limit) {
    if (!g_window || SDL_GetAtomicInt(&g_closed) || SDL_GetAtomicInt(&g_tb_state) == 1) return -1;
    SDL_LockMutex(g_lock);
    snprintf(g_tb_prompt, sizeof g_tb_prompt, "%s", prompt && *prompt ? prompt : "Enter text:");
    char *o = g_tb_text;
    for (int i = 0; initial && initial[i] && o + 4 < g_tb_text + sizeof g_tb_text; i++) {
        const unsigned c = initial[i];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 0x3F)); }
        else { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    }
    *o = 0;
    g_tb_limit = limit > 0 && limit < TEXT_MAX ? limit : TEXT_MAX;
    SDL_SetAtomicInt(&g_tb_state, 1);
    SDL_UnlockMutex(g_lock);
    wake(g_ev_wake);
    return 0;
}

int mhfu_window_text_result(uint16_t *out, int cap) {
    const int s = SDL_GetAtomicInt(&g_tb_state);
    if (s == 1) return SDL_GetAtomicInt(&g_closed) ? -2 : -1;
    if (s != 2) return -2;
    SDL_LockMutex(g_lock);
    int n = 0;
    for (const char *p = g_tb_text; *p && n + 1 < cap;) out[n++] = (uint16_t)utf8_next(&p);
    if (cap > 0) out[n] = 0;
    SDL_UnlockMutex(g_lock);
    return n;
}

void mhfu_window_notify(const char *head, const char *title, const char *text_, const char *badge_png) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (g_ntoasts < TOAST_MAX) {
        toast *t = &g_toasts[g_ntoasts++];
        snprintf(t->head, sizeof t->head, "%s", head ? head : "");
        snprintf(t->title, sizeof t->title, "%s", title ? title : "");
        snprintf(t->text, sizeof t->text, "%s", text_ ? text_ : "");
        snprintf(t->badge, sizeof t->badge, "%s", badge_png ? badge_png : "");
        if (g_ntoasts == 1) { g_toast_t0 = SDL_GetTicks(); g_badge_loaded = 0; }
    }
    SDL_UnlockMutex(g_lock);
    wake(g_ev_wake);
}

/* ---- the loop, on the main thread ------------------------------------------------------- */

static void make_window(void) {
#ifdef __ANDROID__
    g_gles = 1;
#else
    g_gles = getenv("MHFU_GLES") != NULL;
#endif
    if (g_gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    const int w = g_out_w > 0 ? g_out_w : PSP_W * g_scale, h = g_out_h > 0 ? g_out_h : PSP_H * g_scale;
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
    /* Landscape only: a resizable window otherwise lets SDL turn the phone
     * whichever way it likes, over the manifest's sensorLandscape. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (g_fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    g_window = SDL_CreateWindow("MHFU (recompiled)", w, h, flags);
    if (g_window) {
        g_ui = SDL_GetWindowDisplayScale(g_window);
        if (g_ui < 1) g_ui = 1;
        font_init();
    }
    SDL_LockMutex(g_lock);
    g_window_state = g_window ? 1 : -1;
    SDL_SignalCondition(g_cond);
    SDL_UnlockMutex(g_lock);
    if (!g_window) fprintf(stderr, "window: %s\n", SDL_GetError());
}

static void text_event(const SDL_Event *e) {
    SDL_LockMutex(g_lock);
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        const char *in = e->text.text;
        size_t len = strlen(g_tb_text);
        while (*in && utf8_len(g_tb_text) < g_tb_limit) {
            const char *q = in;
            utf8_next(&q);
            if (len + (size_t)(q - in) + 1 >= sizeof g_tb_text) break;
            memcpy(g_tb_text + len, in, (size_t)(q - in));
            len += (size_t)(q - in);
            g_tb_text[len] = 0;
            in = q;
        }
    } else if (e->type == SDL_EVENT_KEY_DOWN) {
        if (e->key.scancode == SDL_SCANCODE_BACKSPACE) {
            size_t len = strlen(g_tb_text);
            while (len && (g_tb_text[len - 1] & 0xC0) == 0x80) len--;    /* to the code point's first byte */
            if (len) len--;
            g_tb_text[len] = 0;
        } else if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) {
            SDL_SetAtomicInt(&g_tb_state, 2);
        } else if (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK) {
            SDL_SetAtomicInt(&g_tb_state, 3);
        }
    }
    SDL_UnlockMutex(g_lock);
}

typedef struct { int (*fn)(int, char **); int argc; char **argv; int code; } game_args;

static int SDLCALL game_thread(void *arg) {
    game_args *g = (game_args *)arg;
    g->code = g->fn(g->argc, g->argv);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
    return g->code;
}

int mhfu_window_main(int (*game)(int, char **), int argc, char **argv) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");      /* Back opens the menu */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "window: SDL: %s\n", SDL_GetError());
        return game(argc, argv);                  /* run without a window */
    }
    g_lock = SDL_CreateMutex();
    g_cond = SDL_CreateCondition();
    g_ev_window = SDL_RegisterEvents(2);
    g_ev_wake = g_ev_window + 1;
    SDL_SetAtomicInt(&g_stick, 0);
    SDL_SetAtomicInt(&g_touch_stick_atomic, -1);
    settings_load();
    g_show_fps = g_show_fps_init;
    mhfu_gamepad_external();

    /* The game, with a big stack: recompiled code nests deeply. */
    static game_args args;
    args.fn = game; args.argc = argc; args.argv = argv;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)game_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &args);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 16 << 20);
    SDL_Thread *t = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!t) { fprintf(stderr, "window: no game thread: %s\n", SDL_GetError()); return 1; }

    uint64_t next_stats = 0, next_title = 0;
    int moving = 0, quit = 0;
    while (!quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, moving ? 16 : 100)) {
            do {
                if (e.type == g_ev_window) { make_window(); continue; }
                if (e.type == g_ev_wake) {
                    if (SDL_GetAtomicInt(&g_tb_state) == 1 && g_window && !SDL_TextInputActive(g_window)) {
                        memset(g_held, 0, sizeof g_held);
                        keys_changed();
                        SDL_StartTextInput(g_window);
                    }
                    moving = 1;
                    continue;
                }
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    quit = 1;
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    SDL_SetAtomicInt(&g_closed, 1);
                    if (g_on_close) g_on_close();   /* reports and exits */
                    quit = 1;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    memset(g_held, 0, sizeof g_held);
                    keys_changed();
                    touch_release_all();
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_SHOWN:
                case SDL_EVENT_WINDOW_RESTORED:
                case SDL_EVENT_WINDOW_EXPOSED:
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    SDL_SetAtomicInt(&g_remake, 1);
                    moving = 1;
                    break;
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_CANCELED: {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
                    const float px = e.tfinger.x * ww, py = e.tfinger.y * wh;
                    if (g_edit) {
                        edit_touch(&e, (float)ww, (float)wh);
                        moving = 1;
                        break;
                    }
                    if (g_menu_open) {
                        menu_touch(&e, px, py, (float)ww, (float)wh);
                        moving = 1;
                        break;
                    }
                    int slot = -1, free_slot = -1;
                    for (int i = 0; i < FINGERS; i++) {
                        if (g_fingers[i].elem && g_fingers[i].id == e.tfinger.fingerID) slot = i;
                        if (!g_fingers[i].elem && free_slot < 0) free_slot = i;
                    }
                    const layout L = layout_for((float)ww, (float)wh);
                    if (e.type == SDL_EVENT_FINGER_DOWN && slot < 0 && free_slot >= 0) {
                        const int el = hit(&L, px, py);
                        if (el == T_MENU) { menu_set(1); moving = 1; break; }
                        if (px < ww * 0.025f) {          /* the left edge: a swipe opens the drawer */
                            g_fingers[free_slot].id = e.tfinger.fingerID;
                            g_fingers[free_slot].elem = T_EDGE;
                            g_fingers[free_slot].x = px;
                            g_fingers[free_slot].y = py;
                            break;
                        }
                        if (el && vpad_visible()) {
                            g_fingers[free_slot].id = e.tfinger.fingerID;
                            g_fingers[free_slot].elem = el;
                            g_fingers[free_slot].x = px;
                            g_fingers[free_slot].y = py;
                        }
                    } else if (slot >= 0 && e.type == SDL_EVENT_FINGER_MOTION && g_fingers[slot].elem == T_EDGE) {
                        if (px - g_fingers[slot].x > ww * 0.05f) { g_fingers[slot].elem = T_NONE; menu_set(1); moving = 1; }
                        break;
                    } else if (slot >= 0 && e.type == SDL_EVENT_FINGER_MOTION) {
                        finger *f = &g_fingers[slot];
                        f->x = px;
                        f->y = py;
                        /* A finger slides from button to button; the stick and
                         * the d-pad keep it wherever it goes. */
                        if (f->elem != T_STICK && f->elem != T_DPAD && f->elem != T_RSTICK) {
                            const int el = hit(&L, px, py);
                            if (el && el != T_MENU && el != T_STICK && el != T_DPAD && el != T_RSTICK) f->elem = el;
                        }
                    } else if (slot >= 0) {
                        g_fingers[slot].elem = T_NONE;
                    }
                    touch_update((float)ww, (float)wh);
                    moving = 1;
                    break;
                }
                case SDL_EVENT_TEXT_INPUT:
                    if (SDL_GetAtomicInt(&g_tb_state) == 1) text_event(&e);
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP: {
                    const int down = e.type == SDL_EVENT_KEY_DOWN;
                    if (SDL_GetAtomicInt(&g_tb_state) == 1) {
                        if (down) text_event(&e);
                        if (SDL_GetAtomicInt(&g_tb_state) != 1) SDL_StopTextInput(g_window);
                        break;
                    }
                    if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F3) { g_show_fps = !g_show_fps; moving = 1; break; }
                    if (down && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_AC_BACK || e.key.scancode == SDL_SCANCODE_F1)) {
                        if (g_edit) edit_close();
                        else menu_set(!g_menu_open);
                        moving = 1;
                        break;
                    }
                    if (down && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_F11 ||
                                                  (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                        g_fullscreen = !g_fullscreen;
                        SDL_SetWindowFullscreen(g_window, g_fullscreen);
                        break;
                    }
                    if (e.key.scancode < SDL_SCANCODE_COUNT) {
                        g_held[e.key.scancode] = (unsigned char)down;
                        keys_changed();
                    }
                    break;
                }
                default:
                    mhfu_gamepad_event(&e);
                    /* With one connected the touch controls step aside (Automatic). */
                    if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) moving = 1;
                    break;
                }
            } while (!quit && SDL_PollEvent(&e));
        }
        mhfu_gamepad_update();
        const uint64_t now = SDL_GetTicks();
        if (now >= next_stats) {
            mhfu_stats_tick(now, g_fps_text, sizeof g_fps_text);
            next_stats = now + 500;
            moving = 1;
        }
        if (moving && g_window) moving = compose();
        if (g_window && now >= next_title) {
            char status[128], title[200];
            mhfu_lobby_status(status, sizeof status);
            snprintf(title, sizeof title, "MHFU (recompiled) - %s", status);
            SDL_SetWindowTitle(g_window, title);
            next_title = now + 2000;
        }
    }
    int code = 0;
    if (!SDL_GetAtomicInt(&g_closed)) SDL_WaitThread(t, &code);
    return code;
}

#endif
