/* MHFU-Android -- a desktop window that shows the PSP framebuffer and turns
 * keys into PSP buttons. Windows only for now; elsewhere these do nothing. */
#ifndef MHFU_WINDOW_H
#define MHFU_WINDOW_H

#include <stdint.h>

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

#endif
