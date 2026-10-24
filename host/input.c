/* MHFU-Android -- buttons for the desktop host. See input.h.
 *
 * psprecomp's sceCtrlReadBufferPositive reports whatever psp_ctrl_set was last
 * given. This one works out the buttons itself on every read -- the script's
 * presses for this frame, plus the window's keys -- so a read is also the
 * frame clock that scripts and snapshots count in. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "psprecomp/hle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "input.h"
#include "window.h"

#define HOLD_FRAMES 6
#define MAX_PRESSES 256

typedef struct { uint64_t from, to; uint32_t buttons; } press;

static press    g_press[MAX_PRESSES];
static int      g_npress;
static uint64_t g_frame;
static int      g_snap_every;
static char     g_snap_dir[512];
static int      g_pacing;

#define FRAME_NS 16666667ll

static long long wall_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* Wait out the rest of this frame's 1/60 s. If the host falls behind, the
 * clock restarts instead of rushing to catch up. The wait yields the CPU: a
 * host sleep here kept it from every other guest thread for most of the frame,
 * because the scheduler is cooperative. */
static void pace(void) {
    static long long next;
    long long now = wall_ns();
    if (!next || now - next > 10 * FRAME_NS) next = now;
    next += FRAME_NS;
    while (wall_ns() < next) psp_threadman_yield();
}

void mhfu_input_set_pacing(int on) { g_pacing = on; }

static const struct { const char *name; uint32_t bit; } k_names[] = {
    { "CROSS", PSP_CTRL_CROSS }, { "X", PSP_CTRL_CROSS },
    { "CIRCLE", PSP_CTRL_CIRCLE }, { "O", PSP_CTRL_CIRCLE },
    { "SQUARE", PSP_CTRL_SQUARE }, { "TRIANGLE", PSP_CTRL_TRIANGLE },
    { "START", PSP_CTRL_START }, { "SELECT", PSP_CTRL_SELECT },
    { "UP", PSP_CTRL_UP }, { "DOWN", PSP_CTRL_DOWN },
    { "LEFT", PSP_CTRL_LEFT }, { "RIGHT", PSP_CTRL_RIGHT },
    { "L", PSP_CTRL_LTRIGGER }, { "R", PSP_CTRL_RTRIGGER },
};

static int parse_buttons(const char *s, size_t n, uint32_t *out) {
    *out = 0;
    while (n) {
        size_t len = 0;
        while (len < n && s[len] != '+') len++;
        int found = 0;
        for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
            const char *nm = k_names[i].name;
            if (strlen(nm) != len) continue;
            size_t j = 0;
            while (j < len && toupper((unsigned char)s[j]) == nm[j]) j++;
            if (j == len) { *out |= k_names[i].bit; found = 1; break; }
        }
        if (!found) return -1;
        s += len; n -= len;
        if (n) { s++; n--; }
    }
    return *out ? 0 : -1;
}

static int parse_script(const char *s) {
    g_npress = 0;
    while (s && *s) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        char *end;
        uint64_t from = strtoull(s, &end, 10), to = from + HOLD_FRAMES - 1;
        if (end == s) return -1;
        s = end;
        if (*s == '-') {
            to = strtoull(s + 1, &end, 10);
            if (end == s + 1 || to < from) return -1;
            s = end;
        }
        if (*s++ != ':') return -1;
        size_t n = strcspn(s, ",");
        uint32_t b;
        if (parse_buttons(s, n, &b) != 0 || g_npress == MAX_PRESSES) return -1;
        g_press[g_npress++] = (press){ from, to, b };
        s += n;
    }
    return 0;
}

static uint32_t script_buttons(uint64_t frame) {
    uint32_t b = 0;
    for (int i = 0; i < g_npress; i++)
        if (frame >= g_press[i].from && frame <= g_press[i].to) b |= g_press[i].buttons;
    return b;
}

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;

    if (g_pacing) pace();
    uint64_t frame = g_frame++;
    uint8_t ax = 128, ay = 128;
    uint32_t buttons = script_buttons(frame) | mhfu_window_buttons(&ax, &ay);
    uint32_t stamp = (uint32_t)psp_threadman_vtime_us();

    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, stamp);
        psp_write32(at + 4, buttons);
        psp_write8(at + 8, ax);
        psp_write8(at + 9, ay);
        for (uint32_t k = 10; k < 16; k++) psp_write8(at + k, 0);
    }

    if (g_snap_every && frame % (uint64_t)g_snap_every == 0) {
        char path[600];
        snprintf(path, sizeof path, "%s/f%06llu.ppm", g_snap_dir, (unsigned long long)frame);
        psp_display_capture(path);
    }
    psp_ret(count);
}

int mhfu_input_init(const char *script, int snap_every, const char *snap_dir) {
    g_frame = 0;
    g_snap_every = snap_every > 0 && snap_dir ? snap_every : 0;
    snprintf(g_snap_dir, sizeof g_snap_dir, "%s", snap_dir ? snap_dir : "");
    if (parse_script(script) != 0) {
        fprintf(stderr, "mhfu: cannot read the input script \"%s\"\n", script);
        return -1;
    }
    if (g_npress) printf("input script: %d press(es)\n", g_npress);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive", hle_ReadBufferPositive);
    return 0;
}

uint64_t mhfu_input_frames(void) { return g_frame; }
