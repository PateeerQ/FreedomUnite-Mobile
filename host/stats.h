/* MHFU-Android -- the FPS line and the run's 10-second log line, shared by
 * both windows (window.c on Win32, window_sdl.c on SDL). */
#ifndef MHFU_STATS_H
#define MHFU_STATS_H

#include <stddef.h>
#include <stdint.h>

/* Call about twice a second with a millisecond clock: writes the FPS line
 * (frames the game showed per second, average and worst frametime) into
 * `fps_text`, and every 10 s logs fps, vblanks, controller reads, audio and
 * ad hoc traffic. */
void mhfu_stats_tick(uint64_t now_ms, char *fps_text, size_t cap);

#endif
