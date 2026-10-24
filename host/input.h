/* MHFU-Android -- buttons for the desktop host: a script of timed presses,
 * the keyboard (with the window), and frame snapshots. */
#ifndef MHFU_INPUT_H
#define MHFU_INPUT_H

#include <stdint.h>

/* PSP button bits, as sceCtrl reports them. */
#define PSP_CTRL_SELECT   0x0001u
#define PSP_CTRL_START    0x0008u
#define PSP_CTRL_UP       0x0010u
#define PSP_CTRL_RIGHT    0x0020u
#define PSP_CTRL_DOWN     0x0040u
#define PSP_CTRL_LEFT     0x0080u
#define PSP_CTRL_LTRIGGER 0x0100u
#define PSP_CTRL_RTRIGGER 0x0200u
#define PSP_CTRL_TRIANGLE 0x1000u
#define PSP_CTRL_CIRCLE   0x2000u
#define PSP_CTRL_CROSS    0x4000u
#define PSP_CTRL_SQUARE   0x8000u

/* Call after psp_hle_init(); replaces sceCtrlReadBufferPositive.
 *
 * `script` is a comma-separated list of FRAME:BUTTONS, where a frame is one
 * controller read (MHFU reads once per game frame) and BUTTONS is names joined
 * by '+' (CROSS, CIRCLE, SQUARE, TRIANGLE, START, SELECT, UP, DOWN, LEFT,
 * RIGHT, L, R). A press is held for 6 frames; FROM-TO:BUTTONS holds it over a
 * range. Example: "300:CROSS,420:START,500-560:UP". NULL or "" for none.
 *
 * Every `snap_every` frames (0: never) the framebuffer is saved to
 * <snap_dir>/f<frame>.ppm. Returns -1 if the script does not parse. */
int mhfu_input_init(const char *script, int snap_every, const char *snap_dir);

/* Hold each frame to at least 1/60 s of wall-clock time, so the game plays
 * at its own speed in the window instead of as fast as the host can go. */
void mhfu_input_set_pacing(int on);

/* Controller reads so far: the game's frame count. */
uint64_t mhfu_input_frames(void);

#endif
