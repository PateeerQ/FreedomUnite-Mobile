/* MHFU-Android -- the desktop window. See window.h.
 *
 * The window lives on its own thread and redraws about 60 times a second
 * from guest memory: whatever sceDisplaySetFrameBuf last pointed at, read
 * while the game runs. A frame can tear; for bring-up that does not matter.
 *
 * Keys (as PPSSPP's defaults): arrows = D-pad, X = Cross, S = Circle,
 * Z = Square, A = Triangle, Q = L, W = R, Space or Enter = Start,
 * V or Backspace = Select, I/J/K/L = analog stick.
 *
 * The Multiplayer menu is "Create Server" / "Join Server" (lobby.h): the
 * room's code and player count show in the title bar. */
#include "window.h"

#ifdef _WIN32

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include "input.h"
#include "lobby.h"

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

static uint32_t key_button(WPARAM k) {
    switch (k) {
    case VK_UP:    return PSP_CTRL_UP;
    case VK_DOWN:  return PSP_CTRL_DOWN;
    case VK_LEFT:  return PSP_CTRL_LEFT;
    case VK_RIGHT: return PSP_CTRL_RIGHT;
    case 'X':      return PSP_CTRL_CROSS;
    case 'S':      return PSP_CTRL_CIRCLE;
    case 'Z':      return PSP_CTRL_SQUARE;
    case 'A':      return PSP_CTRL_TRIANGLE;
    case 'Q':      return PSP_CTRL_LTRIGGER;
    case 'W':      return PSP_CTRL_RTRIGGER;
    case VK_SPACE: case VK_RETURN: return PSP_CTRL_START;
    case 'V':      case VK_BACK:   return PSP_CTRL_SELECT;
    default:       return 0;
    }
}

static LONG key_stick(WPARAM k) {
    switch (k) {
    case 'J': return 1;
    case 'L': return 2;
    case 'I': return 4;
    case 'K': return 8;
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

/* ---- the Multiplayer menu ---- */

enum { ID_CREATE = 100, ID_JOIN, ID_LEAVE, ID_CODE_EDIT, ID_CODE_OK, ID_CODE_CANCEL };
#define WM_LOBBY_DONE (WM_APP + 1)

static HWND          g_main;
static volatile LONG g_lobby_busy;
static char          g_lobby_err[256];
static char          g_join_code[16];

/* Create and join can take a few seconds (finding the room on the LAN), so
 * they run on a thread of their own and post back when done. */
static DWORD WINAPI lobby_worker(LPVOID arg) {
    int create = (int)(INT_PTR)arg;
    g_lobby_err[0] = 0;
    int r = create ? mhfu_lobby_create(g_lobby_err, sizeof g_lobby_err)
                   : mhfu_lobby_join(g_join_code, g_lobby_err, sizeof g_lobby_err);
    PostMessageA(g_main, WM_LOBBY_DONE, (WPARAM)r, (LPARAM)create);
    return 0;
}

static void lobby_start(int create) {
    if (InterlockedExchange(&g_lobby_busy, 1)) return;
    SetWindowTextA(g_main, create ? "MHFU - creating a room..." : "MHFU - looking for the room...");
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

/* The room code prompt: a small window with an edit box. */
static LRESULT CALLBACK code_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND l = CreateWindowA("STATIC", "Room code (e.g. MH-4K7Q):", WS_CHILD | WS_VISIBLE,
                               12, 12, 260, 18, h, NULL, NULL, NULL);
        HWND e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_UPPERCASE,
                                 12, 34, 260, 24, h, (HMENU)ID_CODE_EDIT, NULL, NULL);
        HWND ok = CreateWindowA("BUTTON", "Join", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                110, 68, 78, 26, h, (HMENU)ID_CODE_OK, NULL, NULL);
        HWND cancel = CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    194, 68, 78, 26, h, (HMENU)ID_CODE_CANCEL, NULL, NULL);
        SendMessageA(l, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(e, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(ok, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(cancel, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(e, EM_SETLIMITTEXT, 9, 0);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CODE_OK || LOWORD(wp) == IDOK) {
            GetDlgItemTextA(h, ID_CODE_EDIT, g_join_code, sizeof g_join_code);
            DestroyWindow(h);
            lobby_start(0);
        } else if (LOWORD(wp) == ID_CODE_CANCEL || LOWORD(wp) == IDCANCEL) {
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

static void ask_code(void) {
    static int registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = code_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "mhfu_code";
        RegisterClassA(&wc);
        registered = 1;
    }
    RECT r;
    GetWindowRect(g_main, &r);
    HWND d = CreateWindowExA(WS_EX_DLGMODALFRAME, "mhfu_code", "Join Server",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             r.left + 60, r.top + 80, 300, 140, g_main, NULL, GetModuleHandleA(NULL), NULL);
    if (d) EnableWindow(g_main, FALSE);
}

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
        case ID_CREATE: lobby_start(1); break;
        case ID_JOIN:   if (!g_lobby_busy) ask_code(); break;
        case ID_LEAVE:  mhfu_lobby_leave(); update_title(); break;
        }
        return 0;
    case WM_LOBBY_DONE: {
        InterlockedExchange(&g_lobby_busy, 0);
        update_title();
        if ((int)wp != 0) MessageBoxA(h, g_lobby_err, "Multiplayer", MB_OK | MB_ICONWARNING);
        else if ((int)lp) {
            char code[8], text[200];
            mhfu_lobby_code(code);
            snprintf(text, sizeof text,
                     "Room %s is open.\n\nOther players choose Join Server and enter %s.\n"
                     "Then go to the Gathering Hall as usual.", code, code);
            MessageBoxA(h, text, "Multiplayer", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
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
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_PAINT: {
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

uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) {
    LONG s = g_stick;
    *ax = (s & 1) ? 0 : (s & 2) ? 255 : 128;
    *ay = (s & 4) ? 0 : (s & 8) ? 255 : 128;
    return (uint32_t)g_buttons;
}

#else  /* !_WIN32 */

int mhfu_window_start(int scale) { (void)scale; return -1; }
int mhfu_window_closed(void) { return 0; }
void mhfu_window_on_close(void (*fn)(void)) { (void)fn; }
uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) { (void)ax; (void)ay; return 0; }

#endif
