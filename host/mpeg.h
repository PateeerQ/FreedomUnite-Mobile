#ifndef MHFU_MPEG_H
#define MHFU_MPEG_H

/* sceMpeg with the ringbuffer and stream bookkeeping but no video decoder:
 * movies run their length as black frames and end (host/mpeg.c). */
void mhfu_mpeg_register(void);

#endif
