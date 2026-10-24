/* MHFU-Android -- the desktop window. See window.h.
 *
 * The window lives on its own thread and redraws about 60 times a second
 * from guest memory: whatever sceDisplaySetFrameBuf last pointed at, read
 * while the game runs. A frame can tear; for bring-up that does not matter.
 *
 * Keys (as PPSSPP's defaults): arrows = D-pad, X = Cross, S = Circle,
 * Z = Square, A = Triangle, Q = L, W = R, Space or Enter = Start,
 * V or Backspace = Select, I/J/K/L = analog stick. */
#include "window.h"

#ifdef _WIN32

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include "input.h"

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

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowA("mhfu_host", "MHFU (recompiled)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SetEvent(ready);
    if (!h) { InterlockedExchange(&g_closed, 1); return 1; }
    ShowWindow(h, SW_SHOW);
    SetTimer(h, 1, 16, NULL);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
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
