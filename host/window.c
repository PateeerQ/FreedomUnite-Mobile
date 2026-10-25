/* MHFU-Android -- the desktop window. See window.h.
 *
 * The window lives on its own thread and redraws about 60 times a second
 * from guest memory: whatever sceDisplaySetFrameBuf last pointed at, read
 * while the game runs. A frame can tear; for bring-up that does not matter.
 *
 * Keys come from input.ini (keys.h; the launcher writes it), by default:
 * W/A/S/D = analog stick (movement), arrows = D-pad, K or Space = Cross,
 * L = Circle, J = Square, I = Triangle, Q = L, E = R, Enter = Start, V or
 * Backspace = Select. F3 = FPS line, F11 or Alt+Enter = fullscreen.
 *
 * The Multiplayer menu is "Create Server" / "Join Server" (lobby.h): the
 * room's code and player count show in the title bar. */
#include "window.h"
#include "about.h"

#if defined(_WIN32) && !MHFU_SDL_WINDOW

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include "audio.h"
#include "input.h"
#include "keys.h"
#include "lobby.h"
#include "net/client.h"
#include "stats.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>

#define PSP_W 480
#define PSP_H 272

static volatile LONG g_buttons;
static volatile LONG g_stick;        /* bit 0 left, 1 right, 2 up, 3 down */
static volatile LONG g_closed;
static void        (*g_on_close)(void);
static int           g_scale = 2;
static int           g_fullscreen;   /* borderless over the whole monitor */
static int           g_out_w, g_out_h; /* the window's client size; 0: PSP size times g_scale */
static WINDOWPLACEMENT g_windowed = { sizeof(WINDOWPLACEMENT) };
static HMENU         g_menu;
static uint32_t      g_pixels[PSP_W * PSP_H];
static int           g_gl;           /* the GL backend presents; WM_PAINT keeps out of its way */
static HWND          g_main;

static void gl_overlay_text(const char *text);
static void compose_overlay(void);
static void toast_lock(void);
static void toast_unlock(void);
static int  g_ntoasts;

/* The FPS overlay (F3): frames the game showed per second, and the average
 * and worst frametime, over the last half second. */
static int    g_show_fps = 1;
static char   g_fps_text[96] = "FPS --";
static char   g_overlay_line[160];          /* the FPS line and/or the ping (stats.h) */
static const char *overlay_line(void) {
    mhfu_stats_overlay(g_show_fps, g_fps_text, g_overlay_line, sizeof g_overlay_line);
    return g_overlay_line[0] ? g_overlay_line : NULL;
}
static void update_fps(void) {
    mhfu_stats_tick(GetTickCount64(), g_fps_text, sizeof g_fps_text);
    if (g_gl) gl_overlay_text(overlay_line());
}

/* The key as input.ini names it: Shift, Ctrl and Alt as left or right. */
static unsigned real_vk(WPARAM wp, LPARAM lp) {
    const UINT scan = (UINT)(lp >> 16) & 0xFF;
    const int ext = (lp >> 24) & 1;
    switch (wp) {
    case VK_SHIFT:   return MapVirtualKeyA(scan, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL: return ext ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:    return ext ? VK_RMENU : VK_LMENU;
    default:         return (unsigned)wp;
    }
}

static uint32_t key_button(WPARAM wp, LPARAM lp) { return mhfu_keys_buttons(real_vk(wp, lp)); }
static LONG     key_stick(WPARAM wp, LPARAM lp)  { return mhfu_keys_stick(real_vk(wp, lp)); }

/* Borderless over the monitor the window is on, without the menu (the
 * Multiplayer menu comes back with the window). */
static void set_fullscreen(HWND h, int on) {
    if (on) {
        GetWindowPlacement(h, &g_windowed);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(h, NULL);
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetMenu(h, g_menu);
        SetWindowPlacement(h, &g_windowed);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    g_fullscreen = on;
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
    ID_CREATE = 100, ID_JOIN, ID_LEAVE, ID_ABOUT,
    ID_DLG_OK, ID_DLG_CANCEL, ID_PASS_EDIT, ID_CODE_EDIT, ID_ROOM_LIST, ID_REFRESH, ID_TEXT_EDIT,
    ID_WHERE_NET, ID_WHERE_LAN,
};
#define WM_LOBBY_DONE (WM_APP + 1)
#define WM_ROOMS_DONE (WM_APP + 2)
#define WM_TOAST      (WM_APP + 4)

static volatile LONG g_lobby_busy;
static char          g_lobby_err[256];
static char          g_join_code[16];
static char          g_pass[8];
static int           g_internet;          /* Create Server: on the relay */

/* Create and join can take a few seconds (finding the room on the LAN), so
 * they run on a thread of their own and post back when done. */
static DWORD WINAPI lobby_worker(LPVOID arg) {
    int create = (int)(INT_PTR)arg;
    g_lobby_err[0] = 0;
    int r = create ? mhfu_lobby_create(g_pass, g_internet, g_lobby_err, sizeof g_lobby_err)
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

/* Create Server: where (the internet or this network) and the passcode, if any. */
static LRESULT CALLBACK create_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        const int relay = mhfu_lobby_has_relay(), y = relay ? 32 : 0;
        if (relay) {
            HWND n = control(h, "BUTTON", "Internet", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 12, 10, 100, 20, ID_WHERE_NET, 0);
            control(h, "BUTTON", "This network (LAN)", BS_AUTORADIOBUTTON, 120, 10, 190, 20, ID_WHERE_LAN, 0);
            SendMessageA(n, BM_SETCHECK, BST_CHECKED, 0);
        }
        control(h, "STATIC", "Passcode (4 digits), or empty for a public room:", WS_GROUP, 12, 12 + y, 300, 18, 0, 0);
        HWND e = control(h, "EDIT", "", WS_TABSTOP | ES_NUMBER, 12, 34 + y, 120, 24, ID_PASS_EDIT, WS_EX_CLIENTEDGE);
        SendMessageA(e, EM_SETLIMITTEXT, 4, 0);
        control(h, "BUTTON", "Create", WS_TABSTOP | BS_DEFPUSHBUTTON, 150, 70 + y, 78, 26, ID_DLG_OK, 0);
        control(h, "BUTTON", "Cancel", WS_TABSTOP, 234, 70 + y, 78, 26, ID_DLG_CANCEL, 0);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DLG_OK || LOWORD(wp) == IDOK) {
            if (read_pass(h) != 0) return 0;
            g_internet = mhfu_lobby_has_relay() && IsDlgButtonChecked(h, ID_WHERE_NET) == BST_CHECKED;
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

static void ask_create(void) { open_dialog("mhfu_create", create_proc, "Create Server", 324, mhfu_lobby_has_relay() ? 140 : 108); }
static void ask_join(void)   { open_dialog("mhfu_join", join_proc, "Join Server", 420, 292); }

static HMENU make_menu(void) {
    HMENU bar = CreateMenu(), mp = CreatePopupMenu();
    AppendMenuA(mp, MF_STRING, ID_CREATE, "&Create Server");
    AppendMenuA(mp, MF_STRING, ID_JOIN, "&Join Server...");
    AppendMenuA(mp, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mp, MF_STRING, ID_LEAVE, "&Leave Server");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)mp, "&Multiplayer");
    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, ID_ABOUT, "&About");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CREATE: if (!g_lobby_busy) ask_create(); break;
        case ID_JOIN:   if (!g_lobby_busy) ask_join(); break;
        case ID_LEAVE:  mhfu_lobby_leave(); update_title(); break;
        case ID_ABOUT:
            MessageBoxA(h, MHFU_APP_NAME "\nVersion " MHFU_VERSION "\n\n" MHFU_ABOUT_TEXT, "About", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    case WM_ASK_TEXT:
        if (!open_dialog("mhfu_text", text_proc, strncmp(g_text_prompt, "Chat", 4) ? "Keyboard" : "Chat", 324, 108))
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
            if (g_gl) gl_overlay_text(overlay_line());
            return 0;
        }
        if (!(lp & (1 << 30)) && (wp == VK_F11 || (wp == VK_RETURN && msg == WM_SYSKEYDOWN))) {
            set_fullscreen(h, !g_fullscreen);
            return 0;
        }
        InterlockedOr(&g_buttons, (LONG)key_button(wp, lp));
        InterlockedOr(&g_stick, key_stick(wp, lp));
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        InterlockedAnd(&g_buttons, ~(LONG)key_button(wp, lp));
        InterlockedAnd(&g_stick, ~key_stick(wp, lp));
        return 0;
    case WM_SYSCHAR:
        return 0;                        /* no beep for Alt+Enter */
    case WM_KILLFOCUS:
        InterlockedExchange(&g_buttons, 0);
        InterlockedExchange(&g_stick, 0);
        return 0;
    case WM_TOAST:
        SetTimer(h, 4, 16, NULL);
        return 0;
    case WM_TIMER:
        if (wp == 2) { if (!g_lobby_busy) update_title(); return 0; }
        if (wp == 3) { update_fps(); return 0; }
        if (wp == 4) {                   /* a toast is moving */
            compose_overlay();
            toast_lock();
            const int idle = !g_ntoasts;
            toast_unlock();
            if (idle) KillTimer(h, 4);
            return 0;
        }
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* WIC, for the toasts' badges */
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;                 /* one DC for the life of the window, for GL */
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));      /* the crest (assets/icon/mhfu.rc) */
    wc.lpszClassName = "mhfu_host";
    RegisterClassA(&wc);

    RECT r = { 0, 0, g_out_w > 0 ? g_out_w : PSP_W * g_scale, g_out_h > 0 ? g_out_h : PSP_H * g_scale };
    DWORD style = WS_OVERLAPPEDWINDOW;
    g_menu = make_menu();
    AdjustWindowRect(&r, style, TRUE);
    HWND h = CreateWindowA("mhfu_host", "MHFU (recompiled)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, g_menu, wc.hInstance, NULL);
    g_main = h;
    SetEvent(ready);
    if (!h) { InterlockedExchange(&g_closed, 1); return 1; }
    ShowWindow(h, SW_SHOW);
    if (g_fullscreen) set_fullscreen(h, 1);
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

void mhfu_window_options(int fullscreen, int out_w, int out_h, const char *keys_ini) {
    g_fullscreen = fullscreen;
    g_out_w = out_w;
    g_out_h = out_h;
    mhfu_keys_load(keys_ini);
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
/* ---- the overlay: the FPS line and achievement toasts ----
 *
 * psprecomp draws one image over the top-left corner of the frame, at window
 * pixels. With a toast showing, that image is as wide as the window: the FPS
 * line on the left, the toast's card on the right, transparent between. */

#define TOAST_W     440
#define TOAST_H     116
#define TOAST_MAX   8
#define TOAST_IN_MS 320
#define TOAST_ON_MS 5200

typedef struct {
    char head[64], title[256], text[512], badge[700];
} toast;

static CRITICAL_SECTION g_toast_lock;
static volatile LONG    g_toast_lock_state;  /* 0 new, 1 being set up, 2 ready */
static toast            g_toasts[TOAST_MAX];
static int              g_ntoasts;
static ULONGLONG        g_toast_t0;          /* when the showing one started; 0: none */
static uint32_t        *g_badge;             /* the showing toast's badge, BGRA */
static int              g_badge_w, g_badge_h, g_badge_loaded;
static const char      *g_fps_shown;         /* what gl_overlay_text was last given */

/* The room's chat (chat.c): the last few lines, top-left under the FPS line,
 * each for CHAT_SHOW_MS (all of them while the chat box is open). Under the
 * toasts' lock; the FPS timer recomposes twice a second, which ends them. */
#define CHAT_LINES   8
#define CHAT_SHOW_MS 12000
#define CHAT_W       520
#define CHAT_LINE_H  22
typedef struct { char nick[48], text[256]; int self; ULONGLONG t; } chat_line;
static chat_line g_chat[CHAT_LINES];
static int       g_nchat;

static void toast_lock(void) {
    if (InterlockedCompareExchange(&g_toast_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_toast_lock);
        InterlockedExchange(&g_toast_lock_state, 2);
    }
    while (g_toast_lock_state != 2) Sleep(0);
    EnterCriticalSection(&g_toast_lock);
}
static void toast_unlock(void) { LeaveCriticalSection(&g_toast_lock); }

/* A PNG into BGRA through WIC; NULL if it cannot be read. */
static uint32_t *load_png(const char *path, int *w, int *h) {
    wchar_t wp[700];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 700)) return NULL;
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    uint32_t *px = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        IWICFormatConverter_GetSize(cv, &uw, &uh);
        px = uw && uh ? (uint32_t *)malloc((size_t)uw * uh * 4) : NULL;
        if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, uw * 4, uw * uh * 4, (BYTE *)px))) { free(px); px = NULL; }
        *w = (int)uw;
        *h = (int)uh;
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (f) IWICImagingFactory_Release(f);
    return px;
}

static HFONT font(int px, int weight, const wchar_t *face) {
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static void text_utf8(HDC dc, const char *s, RECT *r, UINT flags) {
    wchar_t w[600];
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 600)) DrawTextW(dc, w, -1, r, flags | DT_NOPREFIX);
}

/* The card: dark wood, a brass double border, the badge in a gold frame,
 * the kind of news in gold, the title, the description. */
static void draw_card(HDC dc, int x, int y, const toast *t) {
    const COLORREF gold = RGB(236, 194, 104), brass = RGB(126, 98, 54);
    RECT r = { x, y, x + TOAST_W, y + TOAST_H };
    HBRUSH bg = CreateSolidBrush(RGB(30, 23, 17));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    HPEN pg = CreatePen(PS_SOLID, 2, gold), pb = CreatePen(PS_SOLID, 1, brass);
    HGDIOBJ op = SelectObject(dc, pg), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x + 1, y + 1, x + TOAST_W, y + TOAST_H);
    SelectObject(dc, pb);
    Rectangle(dc, x + 5, y + 5, x + TOAST_W - 4, y + TOAST_H - 4);
    /* The badge, or a gold diamond when there is none. */
    const int bx = x + 20, by = y + (TOAST_H - 64) / 2, bs = 64;
    if (g_badge) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = g_badge_w;
        bi.bmiHeader.biHeight = -g_badge_h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, bx, by, bs, bs, 0, 0, g_badge_w, g_badge_h, g_badge, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        HBRUSH gb = CreateSolidBrush(gold);
        const POINT d[4] = { { bx + bs / 2, by + 6 }, { bx + bs - 6, by + bs / 2 }, { bx + bs / 2, by + bs - 6 }, { bx + 6, by + bs / 2 } };
        SelectObject(dc, gb);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, d, 4);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        DeleteObject(gb);
    }
    SelectObject(dc, pg);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bx - 2, by - 2, bx + bs + 2, by + bs + 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pg);
    DeleteObject(pb);

    SetBkMode(dc, TRANSPARENT);
    const int tx = bx + bs + 18, tw = x + TOAST_W - 16 - tx;
    HFONT fh = font(12, FW_BOLD, L"Segoe UI"), ft = font(19, FW_SEMIBOLD, L"Segoe UI"), fd = font(14, FW_NORMAL, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, fh);
    SetTextColor(dc, gold);
    RECT rh = { tx, y + 16, tx + tw, y + 32 };
    text_utf8(dc, t->head, &rh, DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, ft);
    SetTextColor(dc, RGB(252, 247, 237));
    RECT rt = { tx, y + 32, tx + tw, y + 58 };
    text_utf8(dc, t->title, &rt, DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, fd);
    SetTextColor(dc, RGB(205, 192, 168));
    RECT rd = { tx, y + 59, tx + tw, y + TOAST_H - 8 };
    text_utf8(dc, t->text, &rd, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
    SelectObject(dc, of);
    DeleteObject(fh);
    DeleteObject(ft);
    DeleteObject(fd);
}

/* How far the showing toast has slid in (0..1): 1 while one shows, 0 for
 * none, -1 when it is done and the next should start. Under the lock. */
static int toast_phase(ULONGLONG now, float *in) {
    if (!g_ntoasts || !g_toast_t0) return 0;
    const ULONGLONG t = now - g_toast_t0;
    float f;
    if (t < TOAST_IN_MS) f = (float)t / TOAST_IN_MS;
    else if (t < TOAST_IN_MS + TOAST_ON_MS) f = 1;
    else if (t < TOAST_IN_MS * 2 + TOAST_ON_MS) f = 1 - (float)(t - TOAST_IN_MS - TOAST_ON_MS) / TOAST_IN_MS;
    else return -1;
    *in = 1 - (1 - f) * (1 - f) * (1 - f);             /* ease out */
    return 1;
}

/* The chat lines to show now, oldest first; how many. Under the lock. */
static int chat_visible(ULONGLONG now, const chat_line **out) {
    const int all = g_text_state == 1;
    int n = 0;
    for (int i = 0; i < g_nchat; i++)
        if (all || now - g_chat[i].t < CHAT_SHOW_MS) out[n++] = &g_chat[i];
    return n;
}

static void draw_chat(HDC dc, int x, int y, const chat_line *const *lines, int n) {
    HFONT fb = font(15, FW_BOLD, L"Segoe UI"), fn = font(15, FW_NORMAL, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, fb);
    HBRUSH bg = CreateSolidBrush(RGB(20, 15, 11));
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < n; i++, y += CHAT_LINE_H) {
        const chat_line *l = lines[i];
        RECT r = { x, y, x + CHAT_W, y + CHAT_LINE_H };
        FillRect(dc, &r, bg);
        int tx = x + 8;
        if (l->self != 2) {                            /* "Name:" in gold (green for mine) */
            char who[64];
            snprintf(who, sizeof who, "%s:", l->nick);
            wchar_t ww[64];
            SIZE sz = { 0, 0 };
            SelectObject(dc, fb);                      /* measured in the font it is drawn in */
            if (MultiByteToWideChar(CP_UTF8, 0, who, -1, ww, 64)) GetTextExtentPoint32W(dc, ww, (int)wcslen(ww), &sz);
            SetTextColor(dc, l->self ? RGB(150, 225, 120) : RGB(236, 194, 104));
            RECT rn = { tx, y + 2, x + CHAT_W / 2, y + CHAT_LINE_H };
            text_utf8(dc, who, &rn, DT_SINGLELINE | DT_END_ELLIPSIS);
            tx += (sz.cx < CHAT_W / 2 - 8 ? sz.cx : CHAT_W / 2 - 8) + 6;
        }
        SelectObject(dc, fn);
        SetTextColor(dc, l->self == 2 ? RGB(205, 192, 168) : RGB(252, 247, 237));
        RECT rt = { tx, y + 2, x + CHAT_W - 6, y + CHAT_LINE_H };
        text_utf8(dc, l->text, &rt, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(dc, of);
    DeleteObject(bg);
    DeleteObject(fb);
    DeleteObject(fn);
}

static void compose_overlay(void) {
    if (!g_gl) return;
    const char *text = g_fps_shown;
    toast_lock();
    const chat_line *chat[CHAT_LINES];
    const int nchat = chat_visible(GetTickCount64(), chat);
    float in = 0;
    int showing = toast_phase(GetTickCount64(), &in);
    while (showing < 0) {                              /* the next toast, with its badge */
        memmove(g_toasts, g_toasts + 1, (size_t)(--g_ntoasts) * sizeof *g_toasts);
        free(g_badge);
        g_badge = NULL;
        g_toast_t0 = g_ntoasts ? GetTickCount64() : 0;
        g_badge_loaded = 0;
        showing = toast_phase(GetTickCount64(), &in);
    }
    if (showing && !g_badge_loaded) {                  /* decoded here, on the window's thread */
        g_badge_loaded = 1;
        if (g_toasts[0].badge[0]) g_badge = load_png(g_toasts[0].badge, &g_badge_w, &g_badge_h);
    }
    if (!showing && !text && !nchat) { toast_unlock(); psp_render_gl_overlay(NULL, 0, 0); return; }

    HDC dc = CreateCompatibleDC(NULL);
    HFONT old = (HFONT)SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
    SIZE sz = { 0, 0 };
    if (text) GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    const int fw = text ? sz.cx + 8 : 0, fh = text ? sz.cy + 4 : 0;
    RECT cr = { 0, 0, 0, 0 };
    GetClientRect(g_main, &cr);
    int w = showing ? (cr.right > fw ? cr.right : fw) : fw;
    int h = showing ? (fh > TOAST_H + 24 ? fh : TOAST_H + 24) : fh;
    const int chx = 6, chy = fh + 6;                    /* the chat, under the FPS line */
    if (nchat) {
        if (w < chx + CHAT_W) w = chx + CHAT_W;
        if (h < chy + nchat * CHAT_LINE_H) h = chy + nchat * CHAT_LINE_H;
    }
    const int cx = w - TOAST_W - 20 + (int)((1 - in) * (TOAST_W + 30)), cy = 20;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = w > 0 && h > 0 ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (bmp && bits) {
        HBITMAP oldb = (HBITMAP)SelectObject(dc, bmp);
        memset(bits, 0, (size_t)w * (size_t)h * 4);
        SetBkMode(dc, TRANSPARENT);
        if (text) {
            SetTextColor(dc, RGB(80, 255, 80));
            TextOutA(dc, 4, 2, text, (int)strlen(text));
        }
        if (showing) draw_card(dc, cx, cy, &g_toasts[0]);
        if (nchat) draw_chat(dc, chx, chy, chat, nchat);
        GdiFlush();
        uint32_t *px = (uint32_t *)bits;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint32_t c = px[y * w + x];          /* 0x00RRGGBB */
                uint32_t a = 0;
                if (x < fw && y < fh) a = c ? 0xFF : 0xC0;                    /* the FPS box */
                if (showing && x >= cx && x < cx + TOAST_W && y >= cy && y < cy + TOAST_H) a = 0xF2;
                if (nchat && x >= chx && x < chx + CHAT_W && y >= chy && y < chy + nchat * CHAT_LINE_H) a = 0xD8;
                px[y * w + x] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF) | a << 24;
            }
        psp_render_gl_overlay(px, w, h);
        SelectObject(dc, oldb);
    }
    toast_unlock();
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, old);
    DeleteDC(dc);
}

static void gl_overlay_text(const char *text) {
    g_fps_shown = text;
    compose_overlay();
}

void mhfu_window_notify(const char *head, const char *title, const char *text, const char *badge_png) {
    toast_lock();
    if (g_ntoasts < TOAST_MAX) {
        toast *t = &g_toasts[g_ntoasts++];
        snprintf(t->head, sizeof t->head, "%s", head ? head : "");
        snprintf(t->title, sizeof t->title, "%s", title ? title : "");
        snprintf(t->text, sizeof t->text, "%s", text ? text : "");
        snprintf(t->badge, sizeof t->badge, "%s", badge_png ? badge_png : "");
        if (g_ntoasts == 1) {
            g_toast_t0 = GetTickCount64();
            free(g_badge);
            g_badge = NULL;
            g_badge_loaded = 0;          /* compose_overlay decodes it, on the window's thread */
        }
    }
    toast_unlock();
    if (g_main) PostMessageA(g_main, WM_TOAST, 0, 0);
}

void mhfu_window_chat(const char *nick, const char *text, int self) {
    toast_lock();
    if (g_nchat == CHAT_LINES) memmove(g_chat, g_chat + 1, sizeof g_chat[0] * (--g_nchat));
    chat_line *l = &g_chat[g_nchat++];
    snprintf(l->nick, sizeof l->nick, "%s", nick ? nick : "");
    snprintf(l->text, sizeof l->text, "%s", text ? text : "");
    l->self = self;
    l->t = GetTickCount64();
    toast_unlock();
    if (g_main) PostMessageA(g_main, WM_TOAST, 0, 0);
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
    gl_overlay_text(overlay_line());
    return 0;
}

uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) {
    LONG s = g_stick;
    *ax = (s & 1) ? 0 : (s & 2) ? 255 : 128;
    *ay = (s & 4) ? 0 : (s & 8) ? 255 : 128;
    return (uint32_t)g_buttons;
}

#elif !MHFU_SDL_WINDOW  /* neither window: stubs */

void mhfu_window_options(int fullscreen, int out_w, int out_h, const char *keys_ini) {
    (void)fullscreen; (void)out_w; (void)out_h; (void)keys_ini;
}
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
void mhfu_window_notify(const char *head, const char *title, const char *text, const char *badge_png) {
    (void)head; (void)title; (void)text; (void)badge_png;
}
void mhfu_window_chat(const char *nick, const char *text, int self) { (void)nick; (void)text; (void)self; }

#endif
