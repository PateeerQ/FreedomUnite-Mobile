/* MHFU-Android -- what the game outputs through sceAudio, played on the host.
 * Desktop Windows uses WinMM; elsewhere output is dropped until the Android
 * backend (AAudio) exists. */
#ifndef MHFU_AUDIO_H
#define MHFU_AUDIO_H

/* Open the output device and hook psprecomp's sceAudio. 0 on success; on
 * failure the game runs silent, as before. */
int mhfu_audio_start(void);

/* Totals since the start: frames the game sent, frames missing when the
 * output needed them (gaps), and frames dropped for running too far ahead. */
#include <stdint.h>
void mhfu_audio_stats(uint64_t *in, uint64_t *short_, uint64_t *dropped);

/* Frames the output has taken (mixed), since the start: it should run at
 * 44100 a second; less, and the channels fill up and drop. */
uint64_t mhfu_audio_out_frames(void);

/* How far behind the game the sound is heard, averaged since the last call:
 * what waits in the channel rings plus what the output device holds. Not
 * counted: psprecomp's own lead on blocking output (35 ms). */
double mhfu_audio_latency_ms(void);

/* Diagnostics: a decoder's first audible frame was made at `decoded_us`
 * (mhfu_clock_us); the log says when sound first reaches sceAudio after it. */
void mhfu_audio_mark(uint64_t decoded_us);

/* Plays the achievement chime over the game's sound. Any thread. */
void mhfu_audio_chime(void);
/* The master volume, 0-100 % of the game's own (MHFU_VOLUME, the launcher's
 * slider; F5/F6 in the Win32 window). */
int  mhfu_audio_volume(void);
void mhfu_audio_set_volume(int percent);

#endif
