/* MHFU-Android -- the desktop window. See window.h.
 *
 * The window lives on its own thread and redraws about 60 times a second
 * from guest memory: whatever sceDisplaySetFrameBuf last pointed at, read
 * while the game runs. A frame can tear; for bring-up that does not matter.
 *
 * Keys: W/A/S/D = analog stick (movement), arrows = D-pad, K or Space =
 * Cross, L = Circle, J = Square, I = Triangle, Q = L, E = R, Enter = Start,
 * V or Backspace = Select, F3 = FPS line.
 *
 * The Multiplayer menu is "Create Server" / "Join Server" (lobby.h): the
 * room's code and player count show in the title bar. */
#include "window.h"

#ifdef _WIN32

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include "audio.h"
#include "input.h"
#include "lobby.h"
#include "net/client.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define PSP_W 480
#define PSP_H 272

static volatile LONG g_buttons;
static volatile LONG g_stick;        /* bit 0 left, 1 right, 2 up, 3 down */
static volatile LONG g_closed;
static void        (*g_on_close)(void);
static int           g_scale = 2;
static uint32_t      g_pixels[PSP_W * PSP_H];
static int           g_gl;           /* the GL backend presents; WM_PAINT keeps out of its way */
static HWND          g_main;

static void gl_overlay_text(const char *text);

/* The FPS overlay (F3): frames the game showed per second, and the average
 * and worst frametime, over the last half second. */
static int    g_show_fps = 1;
static char   g_fps_text[96] = "FPS --";
static void update_fps(void) {
    static uint64_t last_flips;
    static ULONGLONG last_ms;
    uint64_t flips, max_us;
    const ULONGLONG now = GetTickCount64();
    psp_display_frame_stats(&flips, &max_us);
    if (last_ms && now > last_ms) {
        const double dt = (double)(now - last_ms) / 1000.0;
        const double n = (double)(flips - last_flips);
        if (n > 0)
            snprintf(g_fps_text, sizeof g_fps_text, "FPS %.1f   %.1f ms (worst %.1f ms)",
                     n / dt, dt * 1000.0 / n, (double)max_us / 1000.0);
        else
            snprintf(g_fps_text, sizeof g_fps_text, "FPS 0   (no frame for %.1f s)", dt);
    }
    /* The log gets the same every 10 seconds, for runs nobody watches. */
    /* With the vblank waits and controller reads per second too: a game that
     * runs at the wrong speed shows which clock is off. */
    static uint64_t log_flips, log_vblanks, log_reads;
    static ULONGLONG log_ms;
    if (!log_ms) {
        log_ms = now; log_flips = flips; log_vblanks = psp_display_vblanks(); log_reads = mhfu_input_frames();
    } else if (now - log_ms >= 10000) {
        const double s = (double)(now - log_ms) / 1000.0;
        static uint64_t log_ain, log_ashort, log_adrop;
        uint64_t ain, ashort, adrop;
        mhfu_audio_stats(&ain, &ashort, &adrop);
        printf("fps: %.1f over the last %.0f s (vblank waits %.1f/s, controller reads %.1f/s)"
               "  audio: %.0f frames/s in, %llu short, %llu dropped, %.0f ms behind\n",
               (double)(flips - log_flips) / s, s, (double)(psp_display_vblanks() - log_vblanks) / s,
               (double)(mhfu_input_frames() - log_reads) / s, (double)(ain - log_ain) / s,
               (unsigned long long)(ashort - log_ashort), (unsigned long long)(adrop - log_adrop),
               mhfu_audio_latency_ms());
        log_ain = ain; log_ashort = ashort; log_adrop = adrop;
        /* Ad hoc datagrams, once there are any: how long the game left them
         * waiting is the delay other players see. */
        static uint64_t log_pin, log_pread, log_pdrop, log_pwait;
        uint64_t pin, pread, pdrop, pwait, pmax;
        mhn_pdp_stats(&pin, &pread, &pdrop, &pwait, &pmax);
        if (pin != log_pin || pread != log_pread)
            printf("net: PDP %.0f in/s, %.0f read/s, %llu dropped, waited %.0f ms on average (worst %llu ms)\n",
                   (double)(pin - log_pin) / s, (double)(pread - log_pread) / s,
                   (unsigned long long)(pdrop - log_pdrop),
                   pread > log_pread ? (double)(pwait - log_pwait) / (double)(pread - log_pread) : 0.0,
                   (unsigned long long)pmax);
        log_pin = pin; log_pread = pread; log_pdrop = pdrop; log_pwait = pwait;
        fflush(stdout);
        log_ms = now;
        log_flips = flips;
        log_vblanks = psp_display_vblanks();
        log_reads = mhfu_input_frames();
    }
    last_flips = flips;
    last_ms = now;
    if (g_gl) gl_overlay_text(g_show_fps ? g_fps_text : NULL);
}

static uint32_t key_button(WPARAM k) {
    switch (k) {
    case VK_UP:    return PSP_CTRL_UP;
    case VK_DOWN:  return PSP_CTRL_DOWN;
    case VK_LEFT:  return PSP_CTRL_LEFT;
    case VK_RIGHT: return PSP_CTRL_RIGHT;
    case 'K':      case VK_SPACE:  return PSP_CTRL_CROSS;
    case 'L':      return PSP_CTRL_CIRCLE;
    case 'J':      return PSP_CTRL_SQUARE;
    case 'I':      return PSP_CTRL_TRIANGLE;
    case 'Q':      return PSP_CTRL_LTRIGGER;
    case 'E':      return PSP_CTRL_RTRIGGER;
    case VK_RETURN: return PSP_CTRL_START;
    case 'V':      case VK_BACK:   return PSP_CTRL_SELECT;
    default:       return 0;
    }
}

static LONG key_stick(WPARAM k) {
    switch (k) {
    case 'A': return 1;
    case 'D': return 2;
    case 'W': return 4;
    case 'S': return 8;
    default:  return 0;
    }
}

/* The framebuffer, converted to the 32-bit BGRX a DIB wants. */
static void grab(void) {
    uint32_t addr, stride, fmt;
    psp_display_get(&addr, &stride, &fmt);
    if (!addr) { memset(g_pixels, 0, sizeof g_pixels); return; }
    const uint32_t bpp = fmt == 3 ? 4 : 2;
    const uint8_t *src = (const uint8_t *)psp_mem_ptr(addr, stride * PSP_H * bpp);
    if (!src) return;
    for (int y = 0; y < PSP_H; y++) {
        const uint8_t *row = src + (size_t)y * stride * bpp;
        for (int x = 0; x < PSP_W; x++) {
            const uint8_t *p = row + (size_t)x * bpp;
            uint32_t px = bpp == 4 ? (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24
                                   : (uint32_t)(p[0] | p[1] << 8);
            uint32_t c = psp_color_expand(px, (int)fmt);          /* R in the low byte */
            g_pixels[y * PSP_W + x] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
        }
    }
}

/* ---- the Multiplayer menu ----
 *
 * As in Monster Hunter's online lobbies: Create Server asks for an optional
 * 4-digit passcode; Join Server lists the open rooms (this network and the
 * relay), each with its host, player count and a lock if it has a passcode,
 * and the passcode is asked for there. A code typed in joins directly. */

enum {
    ID_CREATE = 100, ID_JOIN, ID_LEAVE,
    ID_DLG_OK, ID_DLG_CANCEL, ID_PASS_EDIT, ID_CODE_EDIT, ID_ROOM_LIST, ID_REFRESH, ID_TEXT_EDIT,
};
#define WM_LOBBY_DONE (WM_APP + 1)
#define WM_ROOMS_DONE (WM_APP + 2)

static volatile LONG g_lobby_busy;
static char          g_lobby_err[256];
static char          g_join_code[16];
static char          g_pass[8];

/* Create and join can take a few seconds (finding the room on the LAN), so
 * they run on a thread of their own and post back when done. */
static DWORD WINAPI lobby_worker(LPVOID arg) {
    int create = (int)(INT_PTR)arg;
    g_lobby_err[0] = 0;
    int r = create ? mhfu_lobby_create(g_pass, g_lobby_err, sizeof g_lobby_err)
                   : mhfu_lobby_join(g_join_code, g_pass, g_lobby_err, sizeof g_lobby_err);
    PostMessageA(g_main, WM_LOBBY_DONE, (WPARAM)r, (LPARAM)create);
    return 0;
}

static void lobby_start(int create) {
    if (InterlockedExchange(&g_lobby_busy, 1)) return;
    SetWindowTextA(g_main, create ? "MHFU - creating a room..." : "MHFU - joining the room...");
    HANDLE t = CreateThread(NULL, 0, lobby_worker, (LPVOID)(INT_PTR)create, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_lobby_busy, 0);
}

static void update_title(void) {
    char status[128], title[200];
    mhfu_lobby_status(status, sizeof status);
    snprintf(title, sizeof title, "MHFU (recompiled) - %s", status);
    SetWindowTextA(g_main, title);
}

/* A control on a dialog, in the GUI font. */
static HWND control(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int h,
                    int id, DWORD ex) {
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static int read_pass(HWND d) {
    GetDlgItemTextA(d, ID_PASS_EDIT, g_pass, sizeof g_pass);
    if (mhfu_lobby_check_passcode(g_pass) == 0) return 0;
    MessageBoxA(d, "A passcode is 4 digits (or empty for a public room).", "Multiplayer", MB_OK | MB_ICONWARNING);
    return -1;
}

/* Create Server: the passcode, if any. */
static LRESULT CALLBACK create_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        control(h, "STATIC", "Passcode (4 digits), or empty for a public room:", 0, 12, 12, 300, 18, 0, 0);
        HWND e = control(h, "EDIT", "", WS_TABSTOP | ES_NUMBER, 12, 34, 120, 24, ID_PASS_EDIT, WS_EX_CLIENTEDGE);
        SendMessageA(e, EM_SETLIMITTEXT, 4, 0);
        control(h, "BUTTON", "Create", WS_TABSTOP | BS_DEFPUSHBUTTON, 150, 70, 78, 26, ID_DLG_OK, 0);
        control(h, "BUTTON", "Cancel", WS_TABSTOP, 234, 70, 78, 26, ID_DLG_CANCEL, 0);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DLG_OK || LOWORD(wp) == IDOK) {
            if (read_pass(h) != 0) return 0;
            DestroyWindow(h);
            lobby_start(1);
        } else if (LOWORD(wp) == ID_DLG_CANCEL || LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* Join Server: the list of rooms, filled by a thread (the LAN takes about a
 * second to answer), then a code and a passcode. */
static mhfu_room g_rooms[32];
static int       g_nrooms;

static DWORD WINAPI list_worker(LPVOID arg) {
    HWND d = (HWND)arg;
    static mhfu_room found[32];
    const int n = mhfu_lobby_list(found, 32);
    PostMessageA(d, WM_ROOMS_DONE, (WPARAM)n, (LPARAM)found);
    return 0;
}

static void refresh_rooms(HWND d) {
    HWND list = GetDlgItem(d, ID_ROOM_LIST);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"Looking for rooms...");
    EnableWindow(GetDlgItem(d, ID_REFRESH), FALSE);
    HANDLE t = CreateThread(NULL, 0, list_worker, d, 0, NULL);
    if (t) CloseHandle(t);
}

static void show_rooms(HWND d, const mhfu_room *found, int n) {
    HWND list = GetDlgItem(d, ID_ROOM_LIST);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    g_nrooms = n < 32 ? n : 32;
    memcpy(g_rooms, found, (size_t)g_nrooms * sizeof *g_rooms);
    for (int i = 0; i < g_nrooms; i++) {
        char line[128];
        snprintf(line, sizeof line, "%-24s %d/4   %s%s", g_rooms[i].name, g_rooms[i].players,
                 g_rooms[i].locked ? "[passcode]  " : "", g_rooms[i].internet ? "(internet)" : "(LAN)");
        SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (!g_nrooms) SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)"No rooms found. Refresh, or type a code below.");
    EnableWindow(GetDlgItem(d, ID_REFRESH), TRUE);
}

static LRESULT CALLBACK join_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        control(h, "STATIC", "Rooms:", 0, 12, 10, 200, 18, 0, 0);
        HWND list = control(h, "LISTBOX", "", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                            12, 30, 396, 150, ID_ROOM_LIST, WS_EX_CLIENTEDGE);
        SendMessageA(list, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        control(h, "BUTTON", "Refresh", WS_TABSTOP, 330, 186, 78, 26, ID_REFRESH, 0);
        control(h, "STATIC", "Room code:", 0, 12, 192, 70, 18, 0, 0);
        HWND code = control(h, "EDIT", "", WS_TABSTOP | ES_UPPERCASE, 86, 188, 110, 24, ID_CODE_EDIT, WS_EX_CLIENTEDGE);
        SendMessageA(code, EM_SETLIMITTEXT, 9, 0);
        control(h, "STATIC", "Passcode:", 0, 12, 224, 70, 18, 0, 0);
        HWND pass = control(h, "EDIT", "", WS_TABSTOP | ES_NUMBER, 86, 220, 110, 24, ID_PASS_EDIT, WS_EX_CLIENTEDGE);
        SendMessageA(pass, EM_SETLIMITTEXT, 4, 0);
        control(h, "BUTTON", "Join", WS_TABSTOP | BS_DEFPUSHBUTTON, 246, 254, 78, 26, ID_DLG_OK, 0);
        control(h, "BUTTON", "Cancel", WS_TABSTOP, 330, 254, 78, 26, ID_DLG_CANCEL, 0);
        g_nrooms = 0;
        refresh_rooms(h);
        return 0;
    }
    case WM_ROOMS_DONE:
        show_rooms(h, (const mhfu_room *)lp, (int)wp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ROOM_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK) {
                const int i = (int)SendMessageA(GetDlgItem(h, ID_ROOM_LIST), LB_GETCURSEL, 0, 0);
                if (i < 0 || i >= g_nrooms) return 0;
                SetDlgItemTextA(h, ID_CODE_EDIT, g_rooms[i].code);
                SetFocus(GetDlgItem(h, g_rooms[i].locked ? ID_PASS_EDIT : ID_DLG_OK));
                if (HIWORD(wp) == LBN_DBLCLK && !g_rooms[i].locked) PostMessageA(h, WM_COMMAND, ID_DLG_OK, 0);
            }
            return 0;
        case ID_REFRESH:
            refresh_rooms(h);
            return 0;
        case ID_DLG_OK: case IDOK: {
            GetDlgItemTextA(h, ID_CODE_EDIT, g_join_code, sizeof g_join_code);
            if (!g_join_code[0]) {
                MessageBoxA(h, "Pick a room, or type its code.", "Multiplayer", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (read_pass(h) != 0) return 0;
            for (int i = 0; i < g_nrooms; i++)
                if (g_rooms[i].locked && !g_pass[0] && !_stricmp(g_rooms[i].code, g_join_code)) {
                    MessageBoxA(h, "This room has a passcode.", "Multiplayer", MB_OK | MB_ICONINFORMATION);
                    SetFocus(GetDlgItem(h, ID_PASS_EDIT));
                    return 0;
                }
            DestroyWindow(h);
            lobby_start(0);
            return 0;
        }
        case ID_DLG_CANCEL: case IDCANCEL:
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* A small modal window of class `cls`, made the first time it is asked for. */
static HWND open_dialog(const char *cls, WNDPROC proc, const char *title, int w, int hgt) {
    WNDCLASSA wc;
    if (!GetClassInfoA(GetModuleHandleA(NULL), cls, &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassA(&wc);
    }
    RECT r, a = { 0, 0, w, hgt };
    GetWindowRect(g_main, &r);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    AdjustWindowRectEx(&a, style, FALSE, WS_EX_DLGMODALFRAME);
    HWND d = CreateWindowExA(WS_EX_DLGMODALFRAME, cls, title, style, r.left + 60, r.top + 60,
                             a.right - a.left, a.bottom - a.top, g_main, NULL, GetModuleHandleA(NULL), NULL);
    if (d) EnableWindow(g_main, FALSE);
    return d;
}

/* ---- the game's on-screen keyboard ----
 *
 * The game thread asks (mhfu_window_ask_text) and polls for the answer; the
 * box itself is made here, on the window's thread. */

#define WM_ASK_TEXT (WM_APP + 3)
#define TEXT_MAX    128

static char             g_text_prompt[128];
static WCHAR            g_text[TEXT_MAX + 1];
static int              g_text_limit;
static volatile LONG    g_text_state;     /* 0 none, 1 open, 2 done, 3 cancelled */

static void text_finish(HWND h, int ok) {
    if (ok) GetDlgItemTextW(h, ID_TEXT_EDIT, g_text, TEXT_MAX + 1);
    InterlockedExchange(&g_text_state, ok ? 2 : 3);
    DestroyWindow(h);
}

static LRESULT CALLBACK text_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        control(h, "STATIC", g_text_prompt, 0, 12, 12, 300, 18, 0, 0);
        HWND e = control(h, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, 12, 34, 300, 24, ID_TEXT_EDIT, WS_EX_CLIENTEDGE);
        SetWindowTextW(e, g_text);
        SendMessageA(e, EM_SETLIMITTEXT, (WPARAM)g_text_limit, 0);
        SendMessageA(e, EM_SETSEL, 0, -1);
        control(h, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 150, 70, 78, 26, ID_DLG_OK, 0);
        control(h, "BUTTON", "Cancel", WS_TABSTOP, 234, 70, 78, 26, ID_DLG_CANCEL, 0);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DLG_OK || LOWORD(wp) == IDOK) text_finish(h, 1);
        else if (LOWORD(wp) == ID_DLG_CANCEL || LOWORD(wp) == IDCANCEL) text_finish(h, 0);
        return 0;
    case WM_CLOSE:
        text_finish(h, 0);
        return 0;
    case WM_DESTROY:
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int mhfu_window_ask_text(const char *prompt, const uint16_t *initial, int limit) {
    if (!g_main || g_closed || g_text_state == 1) return -1;
    snprintf(g_text_prompt, sizeof g_text_prompt, "%s", prompt && *prompt ? prompt : "Enter text:");
    int n = 0;
    if (initial) for (; initial[n] && n < TEXT_MAX; n++) g_text[n] = initial[n];
    g_text[n] = 0;
    g_text_limit = limit > 0 && limit < TEXT_MAX ? limit : TEXT_MAX;
    InterlockedExchange(&g_text_state, 1);
    if (!PostMessageA(g_main, WM_ASK_TEXT, 0, 0)) { InterlockedExchange(&g_text_state, 0); return -1; }
    return 0;
}

int mhfu_window_text_result(uint16_t *out, int cap) {
    const LONG s = g_text_state;
    if (s == 1) return g_closed ? -2 : -1;
    if (s != 2) return -2;
    int n = 0;
    for (; g_text[n] && n + 1 < cap; n++) out[n] = g_text[n];
    if (cap > 0) out[n] = 0;
    return n;
}

static void ask_create(void) { open_dialog("mhfu_create", create_proc, "Create Server", 324, 108); }
static void ask_join(void)   { open_dialog("mhfu_join", join_proc, "Join Server", 420, 292); }

static HMENU make_menu(void) {
    HMENU bar = CreateMenu(), mp = CreatePopupMenu();
    AppendMenuA(mp, MF_STRING, ID_CREATE, "&Create Server");
    AppendMenuA(mp, MF_STRING, ID_JOIN, "&Join Server...");
    AppendMenuA(mp, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mp, MF_STRING, ID_LEAVE, "&Leave Server");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)mp, "&Multiplayer");
    return bar;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CREATE: if (!g_lobby_busy) ask_create(); break;
        case ID_JOIN:   if (!g_lobby_busy) ask_join(); break;
        case ID_LEAVE:  mhfu_lobby_leave(); update_title(); break;
        }
        return 0;
    case WM_ASK_TEXT:
        if (!open_dialog("mhfu_text", text_proc, "Keyboard", 324, 108))
            InterlockedExchange(&g_text_state, 3);   /* the box did not open */
        return 0;
    case WM_LOBBY_DONE: {
        InterlockedExchange(&g_lobby_busy, 0);
        update_title();
        if ((int)wp != 0) MessageBoxA(h, g_lobby_err, "Multiplayer", MB_OK | MB_ICONWARNING);
        else if ((int)lp) {
            char code[8], text[200];
            mhfu_lobby_code(code);
            snprintf(text, sizeof text,
                     "Room %s is open%s.\n\nOther players find it under Join Server%s, or type %s there.\n"
                     "Then go to the Gathering Hall as usual.", code, g_pass[0] ? " with a passcode" : "",
                     g_pass[0] ? " and enter the passcode" : "", code);
            MessageBoxA(h, text, "Multiplayer", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp == VK_F3 && !(lp & (1 << 30))) {
            g_show_fps = !g_show_fps;
            if (g_gl) gl_overlay_text(g_show_fps ? g_fps_text : NULL);
            return 0;
        }
        InterlockedOr(&g_buttons, (LONG)key_button(wp));
        InterlockedOr(&g_stick, key_stick(wp));
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        InterlockedAnd(&g_buttons, ~(LONG)key_button(wp));
        InterlockedAnd(&g_stick, ~key_stick(wp));
        return 0;
    case WM_KILLFOCUS:
        InterlockedExchange(&g_buttons, 0);
        InterlockedExchange(&g_stick, 0);
        return 0;
    case WM_TIMER:
        if (wp == 2) { if (!g_lobby_busy) update_title(); return 0; }
        if (wp == 3) { update_fps(); return 0; }
        if (!g_gl) InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        if (g_gl) { ValidateRect(h, NULL); return 0; }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        grab();
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = PSP_W;
        bi.bmiHeader.biHeight = -PSP_H;          /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        RECT r;
        GetClientRect(h, &r);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, PSP_W, PSP_H,
                      g_pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
        if (g_show_fps) {
            HFONT old = (HFONT)SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
            SIZE sz;
            GetTextExtentPoint32A(dc, g_fps_text, (int)strlen(g_fps_text), &sz);
            RECT box = { 4, 4, 4 + sz.cx + 8, 4 + sz.cy + 4 };
            FillRect(dc, &box, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(80, 255, 80));
            TextOutA(dc, 8, 6, g_fps_text, (int)strlen(g_fps_text));
            SelectObject(dc, old);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        InterlockedExchange(&g_closed, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;                 /* one DC for the life of the window, for GL */
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "mhfu_host";
    RegisterClassA(&wc);

    RECT r = { 0, 0, PSP_W * g_scale, PSP_H * g_scale };
    DWORD style = WS_OVERLAPPEDWINDOW;
    HMENU menu = make_menu();
    AdjustWindowRect(&r, style, TRUE);
    HWND h = CreateWindowA("mhfu_host", "MHFU (recompiled)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, menu, wc.hInstance, NULL);
    g_main = h;
    SetEvent(ready);
    if (!h) { InterlockedExchange(&g_closed, 1); return 1; }
    ShowWindow(h, SW_SHOW);
    SetTimer(h, 1, 16, NULL);
    SetTimer(h, 2, 2000, NULL);      /* the title's player count */
    SetTimer(h, 3, 500, NULL);       /* the FPS overlay */
    update_title();

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        /* Enter and Escape in the room code box. */
        HWND top = GetAncestor(m.hwnd, GA_ROOT);
        if (top && top != h && IsDialogMessageA(top, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (g_on_close) g_on_close();
    return 0;
}

int mhfu_window_start(int scale) {
    g_scale = scale > 0 ? scale : 2;
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_thread, ready, 0, NULL);
    if (!t) return -1;
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    CloseHandle(t);
    return g_closed ? -1 : 0;
}

int mhfu_window_closed(void) { return (int)g_closed; }
void mhfu_window_on_close(void (*fn)(void)) { g_on_close = fn; }

/* ---- OpenGL: the context psprecomp's render thread draws with ---- */

static HDC g_gl_dc;

/* On the render thread: a GL 3.3 core context on the window. */
static int gl_ctx_init(void *user) {
    (void)user;
    g_gl_dc = GetDC(g_main);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ChoosePixelFormat(g_gl_dc, &pfd);
    if (!pf || !SetPixelFormat(g_gl_dc, pf, &pfd)) return -1;

    /* wglCreateContextAttribsARB needs a context to be found with. */
    HGLRC tmp = wglCreateContext(g_gl_dc);
    if (!tmp || !wglMakeCurrent(g_gl_dc, tmp)) return -1;
    typedef HGLRC (WINAPI *create_fn)(HDC, HGLRC, const int *);
    create_fn create = (create_fn)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int attr[] = {
        0x2091, 3,          /* WGL_CONTEXT_MAJOR_VERSION_ARB */
        0x2092, 3,          /* WGL_CONTEXT_MINOR_VERSION_ARB */
        0x9126, 1,          /* WGL_CONTEXT_PROFILE_MASK_ARB: core */
        0
    };
    HGLRC ctx = create ? create(g_gl_dc, NULL, attr) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    if (!ctx || !wglMakeCurrent(g_gl_dc, ctx)) return -1;

    /* No vsync: the game paces itself, and a blocked swap would hold up the
     * frame after it. */
    typedef BOOL (WINAPI *interval_fn)(int);
    interval_fn interval = (interval_fn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (interval) interval(0);
    return 0;
}

static void *gl_proc(void *user, const char *name) {
    (void)user;
    void *p = (void *)wglGetProcAddress(name);
    /* GL 1.1 entry points only come from opengl32.dll itself. */
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        static HMODULE lib;
        if (!lib) lib = LoadLibraryA("opengl32.dll");
        p = lib ? (void *)GetProcAddress(lib, name) : NULL;
    }
    return p;
}

static void gl_swap(void *user) { (void)user; SwapBuffers(g_gl_dc); }

static void gl_size(void *user, int *w, int *h) {
    (void)user;
    RECT r;
    GetClientRect(g_main, &r);
    *w = r.right;
    *h = r.bottom;
}

/* The FPS line as an image for the GL overlay: GDI text on a black box. */
static void gl_overlay_text(const char *text) {
    if (!text) { psp_render_gl_overlay(NULL, 0, 0); return; }
    HDC dc = CreateCompatibleDC(NULL);
    HFONT old = (HFONT)SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
    SIZE sz;
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    const int w = sz.cx + 8, h = sz.cy + 4;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HBITMAP oldb = (HBITMAP)SelectObject(dc, bmp);
        memset(bits, 0, (size_t)w * (size_t)h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(80, 255, 80));
        TextOutA(dc, 4, 2, text, (int)strlen(text));
        GdiFlush();
        uint32_t *px = (uint32_t *)bits;
        for (int i = 0; i < w * h; i++) {
            const uint32_t c = px[i];        /* 0x00RRGGBB */
            px[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF) | (c ? 0xFF000000u : 0xC0000000u);
        }
        psp_render_gl_overlay(px, w, h);
        SelectObject(dc, oldb);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, old);
    DeleteDC(dc);
}

int mhfu_window_gl(void) {
    if (!g_main) return -1;
    static psp_gl_host host;
    memset(&host, 0, sizeof host);
    host.init = gl_ctx_init;
    host.get_proc = gl_proc;
    host.swap = gl_swap;
    host.size = gl_size;
    if (psp_render_gl_start(&host, g_scale) != 0) return -1;
    if (psp_render_select("gl") != 0) return -1;
    g_gl = 1;
    gl_overlay_text(g_show_fps ? g_fps_text : NULL);
    return 0;
}

uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) {
    LONG s = g_stick;
    *ax = (s & 1) ? 0 : (s & 2) ? 255 : 128;
    *ay = (s & 4) ? 0 : (s & 8) ? 255 : 128;
    return (uint32_t)g_buttons;
}

#else  /* !_WIN32 */

int mhfu_window_start(int scale) { (void)scale; return -1; }
int mhfu_window_gl(void) { return -1; }
int mhfu_window_closed(void) { return 0; }
void mhfu_window_on_close(void (*fn)(void)) { (void)fn; }
uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) { (void)ax; (void)ay; return 0; }
int mhfu_window_ask_text(const char *prompt, const uint16_t *initial, int limit) {
    (void)prompt; (void)initial; (void)limit;
    return -1;
}
int mhfu_window_text_result(uint16_t *out, int cap) { (void)out; (void)cap; return -2; }

#endif
