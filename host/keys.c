/* MHFU-Android -- see keys.h. */
#include "keys.h"
#include "input.h"      /* PSP_CTRL_* */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; uint32_t button; int stick; } action;

static const action k_actions[] = {
    { "cross",    PSP_CTRL_CROSS,    0 }, { "circle",   PSP_CTRL_CIRCLE,   0 },
    { "square",   PSP_CTRL_SQUARE,   0 }, { "triangle", PSP_CTRL_TRIANGLE, 0 },
    { "l",        PSP_CTRL_LTRIGGER, 0 }, { "r",        PSP_CTRL_RTRIGGER, 0 },
    { "start",    PSP_CTRL_START,    0 }, { "select",   PSP_CTRL_SELECT,   0 },
    { "up",       PSP_CTRL_UP,       0 }, { "down",     PSP_CTRL_DOWN,     0 },
    { "left",     PSP_CTRL_LEFT,     0 }, { "right",    PSP_CTRL_RIGHT,    0 },
    { "stick_up", 0, 4 }, { "stick_down", 0, 8 }, { "stick_left", 0, 1 }, { "stick_right", 0, 2 },
};
#define NACTIONS (int)(sizeof k_actions / sizeof k_actions[0])

/* Muhammad's layout: the defaults when there is no input.ini. */
static const char *const k_defaults[NACTIONS] = {
    "k, space", "l", "j", "i", "q", "e", "enter", "v, backspace",
    "up", "down", "left", "right", "w", "s", "a", "d",
};

/* Windows virtual-key codes by name. */
static const struct { const char *name; unsigned vk; } k_names[] = {
    { "space", 0x20 }, { "enter", 0x0D }, { "backspace", 0x08 }, { "tab", 0x09 }, { "escape", 0x1B },
    { "capslock", 0x14 }, { "lshift", 0xA0 }, { "rshift", 0xA1 }, { "lctrl", 0xA2 }, { "rctrl", 0xA3 },
    { "lalt", 0xA4 }, { "ralt", 0xA5 }, { "left", 0x25 }, { "up", 0x26 }, { "right", 0x27 }, { "down", 0x28 },
    { "insert", 0x2D }, { "delete", 0x2E }, { "home", 0x24 }, { "end", 0x23 }, { "pageup", 0x21 },
    { "pagedown", 0x22 }, { "comma", 0xBC }, { "period", 0xBE }, { "slash", 0xBF }, { "semicolon", 0xBA },
    { "quote", 0xDE }, { "minus", 0xBD }, { "equals", 0xBB }, { "lbracket", 0xDB }, { "rbracket", 0xDD },
    { "backslash", 0xDC }, { "grave", 0xC0 }, { "kp_multiply", 0x6A }, { "kp_plus", 0x6B },
    { "kp_minus", 0x6D }, { "kp_period", 0x6E }, { "kp_divide", 0x6F },
};

static uint32_t g_buttons[256];
static uint8_t  g_stick[256];

static int vk_of(const char *name) {
    const size_t n = strlen(name);
    if (n == 1 && name[0] >= 'a' && name[0] <= 'z') return toupper((unsigned char)name[0]);
    if (n == 1 && name[0] >= '0' && name[0] <= '9') return name[0];
    if (name[0] == 'f' && n >= 2 && n <= 3) {
        const int k = atoi(name + 1);
        if (k >= 1 && k <= 12) return 0x70 + k - 1;
    }
    if (!strncmp(name, "kp", 2) && n == 3 && name[2] >= '0' && name[2] <= '9') return 0x60 + name[2] - '0';
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (!strcmp(k_names[i].name, name)) return (int)k_names[i].vk;
    return -1;
}

/* Binds every key in the comma list `keys` to action `a`. */
static void bind(int a, const char *keys) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", keys);
    for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
        for (char *p = tok; *p; p++) *p = (char)tolower((unsigned char)*p);
        const int vk = vk_of(tok);
        if (vk < 0 || vk > 255) { fprintf(stderr, "input.ini: unknown key \"%s\"\n", tok); continue; }
        g_buttons[vk] |= k_actions[a].button;
        g_stick[vk] |= (uint8_t)k_actions[a].stick;
    }
}

void mhfu_keys_load(const char *path) {
    const char *keys[NACTIONS];
    static char lines[NACTIONS][256];
    for (int a = 0; a < NACTIONS; a++) keys[a] = k_defaults[a];
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *name = line, *val = eq + 1;
            while (isspace((unsigned char)*name)) name++;
            for (char *e = name + strlen(name); e > name && isspace((unsigned char)e[-1]); ) *--e = 0;
            for (char *e = val + strlen(val); e > val && isspace((unsigned char)e[-1]); ) *--e = 0;
            for (int a = 0; a < NACTIONS; a++)
                if (!strcmp(name, k_actions[a].name)) {
                    snprintf(lines[a], sizeof lines[a], "%s", val);
                    keys[a] = lines[a];
                }
        }
        fclose(f);
        printf("keys: %s\n", path);
    }
    memset(g_buttons, 0, sizeof g_buttons);
    memset(g_stick, 0, sizeof g_stick);
    for (int a = 0; a < NACTIONS; a++) bind(a, keys[a]);
}

int mhfu_keys_vk(const char *name) { return name ? vk_of(name) : -1; }
uint32_t mhfu_keys_buttons(unsigned vk) { return vk < 256 ? g_buttons[vk] : 0; }
int      mhfu_keys_stick(unsigned vk)   { return vk < 256 ? g_stick[vk] : 0; }
