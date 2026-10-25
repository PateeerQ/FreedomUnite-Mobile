/* MHFU-Android -- controllers (DualSense, DualShock 4, Xbox and others),
 * through SDL3, on a thread of its own or in the SDL window's loop.
 *
 * Buttons sit where they do on a PSP: Cross/Circle/Square/Triangle by
 * position, L1/L2 = L, R1/R2 = R, Options = Start, Create/Share = Select,
 * D-pad = D-pad, left stick = the analog stick. The right stick works the
 * D-pad too, which is MHFU's camera while hunting. */
#ifndef MHFU_GAMEPAD_H
#define MHFU_GAMEPAD_H

#include <stdint.h>

#if MHFU_GAMEPAD

/* Start watching for controllers; one is used at a time, plugged in or
 * removed whenever. 0 on success. */
int mhfu_gamepad_start(void);

/* PSP buttons held on the controller. The stick is written to *ax/*ay (0-255,
 * 128 centre) only when it is pushed, so the keyboard's stick still works
 * when it is not. */
uint32_t mhfu_gamepad_read(uint8_t *ax, uint8_t *ay);

/* For a window that runs SDL's event loop itself (window_sdl.c): call
 * mhfu_gamepad_external before mhfu_gamepad_start (no thread of its own is
 * started then), hand it every event, and call update once a loop. */
union SDL_Event;
void mhfu_gamepad_external(void);
void mhfu_gamepad_event(const union SDL_Event *e);
void mhfu_gamepad_update(void);
/* A controller is in use (the touch controls hide then, by default). */
int mhfu_gamepad_connected(void);

#else

static inline int mhfu_gamepad_start(void) { return -1; }
static inline uint32_t mhfu_gamepad_read(uint8_t *ax, uint8_t *ay) { (void)ax; (void)ay; return 0; }
static inline int mhfu_gamepad_connected(void) { return 0; }

#endif
#endif
