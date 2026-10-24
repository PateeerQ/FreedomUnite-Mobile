/* MHFU-Android -- a desktop window that shows the PSP framebuffer and turns
 * keys into PSP buttons. Windows only for now; elsewhere these do nothing. */
#ifndef MHFU_WINDOW_H
#define MHFU_WINDOW_H

#include <stdint.h>

/* Open the window on its own thread, `scale` times 480x272. 0 on success. */
int mhfu_window_start(int scale);

/* Nonzero once the user has closed the window. */
int mhfu_window_closed(void);

/* Run `fn` on the window's thread once the window is closed. The guest may
 * never hand the boot thread the CPU again, so the host cannot rely on
 * noticing the close itself; `fn` is expected to report and exit. */
void mhfu_window_on_close(void (*fn)(void));

/* Buttons held right now, and the analog stick (128 is centre). 0 and
 * centre with no window. */
uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay);

#endif
