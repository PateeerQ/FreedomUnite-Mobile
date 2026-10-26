/* MHFU-Android -- see chat.h. */
#include "chat.h"

#include <stdio.h>
#include <string.h>

#include "input.h"
#include "lobby.h"
#include "window.h"
#include "net/client.h"

#define CHAT_CHARS 100          /* what the box takes; well under MHN_CHAT_MAX bytes in UTF-8 */

static int  g_open;             /* the chat box is up */
static int  g_was_online;
static char g_code[8];
static int  g_roll_call;        /* frames until "In the room: ..." (the hub names them first) */

/* UTF-16 (the text box's) into UTF-8. */
static void to_utf8(const uint16_t *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        uint32_t c = *in;
        if (c >= 0xD800 && c < 0xDC00 && in[1] >= 0xDC00 && in[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[1] - 0xDC00);
            in++;
        }
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | c >> 6); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out[n++] = (char)(0xE0 | c >> 12); out[n++] = (char)(0x80 | (c >> 6 & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | c >> 18); out[n++] = (char)(0x80 | (c >> 12 & 0x3F));
            out[n++] = (char)(0x80 | (c >> 6 & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
}

/* Control characters out (someone else's line is shown as text, nothing more),
 * spaces off both ends. */
static void tidy(char *s) {
    for (char *p = s; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7F) *p = ' ';
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    size_t lead = strspn(s, " ");
    if (lead) memmove(s, s + lead, n - lead + 1);
}

/* Lines that came in, to the window; someone coming or going is a notice. */
static void show_incoming(void) {
    char nick[MHN_NICK_LEN], text[MHN_CHAT_MAX + 1];
    int joined;
    while (mhn_chat_next(nick, sizeof nick, text, sizeof text, &joined)) {
        tidy(nick);
        tidy(text);
        if (joined == 2) {                              /* the room's frame rate: news to one who wants 60 */
            if (mhfu_fps_preferred() < 60) continue;
            char notice[MHN_NICK_LEN + 48];
            if (!strcmp(text, "60")) snprintf(notice, sizeof notice, "Room runs at 60 FPS");
            else if (nick[0]) snprintf(notice, sizeof notice, "Room runs at 30 FPS: %s plays at 30", nick);
            else snprintf(notice, sizeof notice, "Room runs at 30 FPS");
            mhfu_window_chat("", notice, 2);
        } else if (joined) {
            char notice[MHN_NICK_LEN + 32];
            snprintf(notice, sizeof notice, "%s %s the server", nick[0] ? nick : "Hunter", joined > 0 ? "joined" : "left");
            mhfu_window_chat("", notice, 2);
        } else if (text[0]) {
            mhfu_window_chat(nick[0] ? nick : "Hunter", text, 0);
        }
    }
}

/* Who was already here, once the hub has named them. */
static void roll_call(void) {
    mhn_peer m[MHN_MAX_MEMBERS];
    const int n = mhn_client_members(m, MHN_MAX_MEMBERS);
    char line[256] = "Nobody else is here yet";
    if (n) {
        size_t k = (size_t)snprintf(line, sizeof line, "In the room:");
        for (int i = 0; i < n && k < sizeof line; i++) {
            tidy(m[i].nick);
            k += (size_t)snprintf(line + k, sizeof line - k, "%s %s", i ? "," : "", m[i].nick[0] ? m[i].nick : "Hunter");
        }
    }
    mhfu_window_chat("", line, 2);
}

uint32_t mhfu_chat_buttons(uint32_t buttons, uint8_t *ax, uint8_t *ay) {
    const int online = mhn_client_online();
    char code[8] = "";
    if (online) mhfu_lobby_code(code);
    /* In a room now, out of it, or straight into another one. */
    if (online != g_was_online || (online && strcmp(code, g_code) != 0)) {
        g_was_online = online;
        if (online) {
            memcpy(g_code, code, sizeof g_code);
            char hint[96];
            snprintf(hint, sizeof hint, "Room %s: press SELECT to chat", g_code);
            mhfu_window_chat("", hint, 2);
            g_roll_call = 60;                           /* 2 s of game frames */
        } else {
            char bye[64];
            snprintf(bye, sizeof bye, "You left room %s", g_code);
            mhfu_window_chat("", bye, 2);
            g_roll_call = 0;
        }
    }
    if (online) {
        if (g_roll_call && --g_roll_call == 0) roll_call();
        show_incoming();
    }

    if (g_open) {
        uint16_t w[CHAT_CHARS + 1];
        const int r = mhfu_window_text_result(w, CHAT_CHARS + 1);
        if (r == -1) {                                  /* still typing: the game sees nothing */
            *ax = *ay = 128;
            return 0;
        }
        g_open = 0;
        if (r >= 0 && online) {
            char text[CHAT_CHARS * 4 + 1];
            to_utf8(w, text, sizeof text);
            tidy(text);
            if (text[0] && mhn_chat_send(text) == 0) {
                printf("chat: sent %u bytes\n", (unsigned)strlen(text));
                mhfu_window_chat(mhfu_lobby_nick(), text, 1);
            }
        }
        *ax = *ay = 128;
        return 0;                                       /* this frame too: Enter may still be down */
    }

    if (!online) return buttons;
    static uint32_t held;
    const int pressed = (buttons & PSP_CTRL_SELECT) && !(held & PSP_CTRL_SELECT);
    held = buttons;
    if (pressed) {
        char prompt[64];
        snprintf(prompt, sizeof prompt, "Chat: room %s (Enter sends, Esc cancels)", g_code);
        static const uint16_t empty[1] = { 0 };
        if (mhfu_window_ask_text(prompt, empty, CHAT_CHARS) == 0) g_open = 1;
    }
    return buttons & ~PSP_CTRL_SELECT;                  /* in a room, Select is the chat's */
}
