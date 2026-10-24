/* MHFU-Android -- what the game outputs through sceAudio, played on the host.
 * Desktop Windows uses WinMM; elsewhere output is dropped until the Android
 * backend (AAudio) exists. */
#ifndef MHFU_AUDIO_H
#define MHFU_AUDIO_H

/* Open the output device and hook psprecomp's sceAudio. 0 on success; on
 * failure the game runs silent, as before. */
int mhfu_audio_start(void);

#endif
