/* MHFU-Android -- a desktop window that shows the PSP framebuffer and turns
 * keys into PSP buttons. Windows only for now; elsewhere these do nothing. */
#ifndef MHFU_WINDOW_H
#define MHFU_WINDOW_H

#include <stdint.h>

/* The SDL window (window_sdl.c, MHFU_SDL_WINDOW) keeps SDL on the main
 * thread, as SDL wants, and runs `game` on a thread of its own; returns its
 * exit code. main() hands itself to this. The Win32 window does not use it. */
int mhfu_window_main(int (*game)(int, char **), int argc, char **argv);

/* Before mhfu_window_start: start borderless fullscreen (F11 and Alt+Enter
 * toggle it), the window's client size (0, 0: 480x272 times the scale), and
 * the key bindings file (keys.h; NULL: the defaults). */
void mhfu_window_options(int fullscreen, int out_w, int out_h, const char *keys_ini);

/* Open the window on its own thread, `scale` times 480x272. 0 on success. */
int mhfu_window_start(int scale);

/* Draw with psprecomp's GL backend into the window, at the window's scale.
 * After mhfu_window_start. 0 on success; otherwise the renderer is unchanged. */
int mhfu_window_gl(void);

/* Nonzero once the user has closed the window. */
int mhfu_window_closed(void);

/* Run `fn` on the window's thread once the window is closed. The guest may
 * never hand the boot thread the CPU again, so the host cannot rely on
 * noticing the close itself; `fn` is expected to report and exit. */
void mhfu_window_on_close(void (*fn)(void));

/* Buttons held right now, and the analog stick (128 is centre). 0 and
 * centre with no window. */
uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay);

/* The game's on-screen keyboard: a text box over the window, with `prompt`
 * above it, `initial` in it (UTF-16, NUL-terminated) and at most `limit`
 * characters. It does not block: 0 once the box is up, -1 with no window. */
int mhfu_window_ask_text(const char *prompt, const uint16_t *initial, int limit);

/* The box's answer: -1 while it is open, -2 if it was cancelled, otherwise
 * the number of characters written to `out` (NUL-terminated, `cap` with it). */
int mhfu_window_text_result(uint16_t *out, int cap);

/* A toast in the window's top-right corner (achievements): a small gold
 * heading, a title, a line or two of text, and a badge (a PNG file; NULL for
 * none). Any thread; they queue and show one at a time. */
void mhfu_window_notify(const char *head, const char *title, const char *text, const char *badge_png);

/* A line of the room's chat (chat.c), shown over the game's top-left for a
 * while: `self` 1 for this player's own, 2 for a notice (no name). Any thread. */
void mhfu_window_chat(const char *nick, const char *text, int self);

#endif
