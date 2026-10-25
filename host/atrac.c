/* MHFU-Android -- sceAtrac3plus: the game's music.
 *
 * The game hands a whole ATRAC3/ATRAC3plus file (RIFF WAVE) to
 * SetDataAndGetID and then pulls PCM from DecodeData. The header is parsed
 * for channels, frame size, sample count, the first sample's offset and loop
 * points; positions, loops and the end flag advance as they would on the PSP.
 * With FFmpeg (MHFU_FFMPEG) each frame is really decoded (atrac3p / atrac3);
 * without it, the music is silence and everything else still runs its course.
 * Unimplemented, these calls returned 0 without writing their outputs, and the
 * village went on with garbage (a return to a data address).
 *
 * Signatures and error codes follow PPSSPP's Core/HLE/sceAtrac.cpp. */
#include "atrac.h"

#include <stdio.h>
#include <string.h>

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#if MHFU_FFMPEG
#include <libavcodec/avcodec.h>
#endif

#define MAX_IDS 6

#define ATRAC_ERROR_BAD_ID            0x80630005u
#define ATRAC_ERROR_UNKNOWN_FORMAT    0x80630006u
#define ATRAC_ERROR_NO_ATRACID        0x80630003u
#define ATRAC_ERROR_ALL_DATA_DECODED  0x80630024u
#define ATRAC_ERROR_NO_LOOP_INFORMATION 0x80630021u

typedef struct {
    int used;
    uint32_t buf, size;
    int channels, plus;          /* plus: ATRAC3plus (2048 samples a frame), else ATRAC3 (1024) */
    uint32_t block, rate;
    int32_t end_sample;          /* last sample index */
    int32_t loop_start, loop_end;/* -1 when the file has no loop */
    int32_t pos;
    int loop_num;                /* -1 forever, 0 none, n times */
    uint32_t data_off, data_size;/* the frames, from the file's start */
    int32_t skip;                /* samples before sample 0 (fact's second word) */
    uint32_t fmt_off, fmt_len;   /* the fmt chunk, for ATRAC3's codec parameters */
#if MHFU_FFMPEG
    AVCodecContext *dec;
    AVFrame  *frame;
    AVPacket *pkt;
    int32_t  have;               /* the frame decoded into pcm, or -1 */
    int      failed;
    int16_t  pcm[2048 * 2];
#endif
} atrac;

static atrac g_at[MAX_IDS];

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int spf(const atrac *a) { return a->plus ? 2048 : 1024; }

static int parse(atrac *a, const uint8_t *d, uint32_t n) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    uint32_t off = 12, data_size = 0;
    int have_fmt = 0, have_fact = 0;
    a->loop_start = a->loop_end = -1;
    while (off + 8 <= n) {
        uint32_t len = rd32(d + off + 4);
        const uint8_t *c = d + off + 8;
        if (!memcmp(d + off, "fmt ", 4) && len >= 16 && off + 8 + 16 <= n) {
            uint16_t tag = rd16(c);
            a->plus = tag == 0xFFFE;              /* WAVE_FORMAT_EXTENSIBLE: ATRAC3plus */
            if (tag != 0xFFFE && tag != 0x0270) return -1;
            a->channels = rd16(c + 2);
            a->rate = rd32(c + 4);
            a->block = rd16(c + 12);
            a->fmt_off = off + 8;
            a->fmt_len = len;
            have_fmt = 1;
        } else if (!memcmp(d + off, "fact", 4) && len >= 4 && off + 12 <= n) {
            a->end_sample = (int32_t)rd32(c) - 1;
            if (len >= 8) a->skip = (int32_t)rd32(c + 4);
            have_fact = 1;
        } else if (!memcmp(d + off, "smpl", 4) && len >= 36 + 24 && off + 8 + 60 <= n) {
            if (rd32(c + 28) > 0) {               /* loop count */
                a->loop_start = (int32_t)rd32(c + 36 + 8);
                a->loop_end = (int32_t)rd32(c + 36 + 12);
            }
        } else if (!memcmp(d + off, "data", 4)) {
            data_size = len;
            a->data_off = off + 8;
            a->data_size = len;
            break;
        }
        off += 8 + len + (len & 1);
    }
    if (!have_fmt || a->channels < 1 || a->channels > 2 || !a->block) return -1;
    if (!have_fact) {
        uint32_t frames = data_size / a->block;
        a->end_sample = (int32_t)(frames * (uint32_t)spf(a)) - 1;
    }
    return 0;
}

#if MHFU_FFMPEG
static void dec_close(atrac *a) {
    avcodec_free_context(&a->dec);
    av_frame_free(&a->frame);
    av_packet_free(&a->pkt);
}

static int dec_open(atrac *a) {
    if (a->failed) return -1;
    if (a->dec) return 0;
    const AVCodec *codec = avcodec_find_decoder(a->plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
    a->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (a->dec) {
        a->dec->sample_rate = (int)a->rate;
        av_channel_layout_default(&a->dec->ch_layout, a->channels);
        a->dec->block_align = (int)a->block;
        /* ATRAC3 takes its parameters from the fmt chunk's extra bytes, as
         * FFmpeg's WAV reader would hand them over. */
        const uint8_t *fmt = (const uint8_t *)psp_mem_ptr(a->buf + a->fmt_off, a->fmt_len);
        if (!a->plus && fmt && a->fmt_len >= 18) {
            uint32_t extra = rd16(fmt + 16);
            if (extra > a->fmt_len - 18) extra = a->fmt_len - 18;
            a->dec->extradata = (uint8_t *)av_mallocz(extra + AV_INPUT_BUFFER_PADDING_SIZE);
            if (a->dec->extradata) {
                memcpy(a->dec->extradata, fmt + 18, extra);
                a->dec->extradata_size = (int)extra;
            }
        }
    }
    if (!a->dec || avcodec_open2(a->dec, codec, NULL) < 0) {
        fprintf(stderr, "atrac: no %s decoder; the music is silent\n", a->plus ? "ATRAC3plus" : "ATRAC3");
        dec_close(a);
        a->failed = 1;
        return -1;
    }
    a->frame = av_frame_alloc();
    a->pkt = av_packet_alloc();
    a->have = -1;
    return 0;
}

/* Decode frame f; with keep, into a->pcm as interleaved 16-bit. */
static int dec_feed(atrac *a, int32_t f, int keep) {
    const uint32_t off = a->data_off + (uint32_t)f * a->block;
    if (f < 0 || off + a->block > a->data_off + a->data_size || off + a->block > a->size) return -1;
    uint8_t *data = (uint8_t *)psp_mem_ptr(a->buf + off, a->block);
    if (!data) return -1;
    a->pkt->data = data;
    a->pkt->size = (int)a->block;
    if (avcodec_send_packet(a->dec, a->pkt) < 0 || avcodec_receive_frame(a->dec, a->frame) < 0) return -1;
    if (!keep) return 0;
    const AVFrame *fr = a->frame;
    const int chans = fr->ch_layout.nb_channels, n = fr->nb_samples < spf(a) ? fr->nb_samples : spf(a);
    memset(a->pcm, 0, sizeof a->pcm);
    for (int i = 0; i < n; i++)
        for (int ch = 0; ch < a->channels; ch++) {
            const int src = ch < chans ? ch : 0;
            float v = fr->format == AV_SAMPLE_FMT_FLTP ? ((const float *)fr->extended_data[src])[i]
                    : fr->format == AV_SAMPLE_FMT_FLT ? ((const float *)fr->data[0])[i * chans + src] : 0.0f;
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            a->pcm[i * a->channels + ch] = (int16_t)(v * 32767.0f);
        }
    return 0;
}

/* Frame f into a->pcm. A frame that does not follow the last one (a loop, a
 * seek) starts the decoder afresh, primed with the frame before it, so its
 * overlap is not heard as a click. */
static int decode_frame(atrac *a, int32_t f) {
    if (f == a->have) return 0;
    if (dec_open(a) != 0) return -1;
    if (f != a->have + 1) {
        avcodec_flush_buffers(a->dec);
        if (f > 0) dec_feed(a, f - 1, 0);
    }
    a->have = -1;
    if (dec_feed(a, f, 1) != 0) return -1;
    a->have = f;
    return 0;
}

/* n samples from sample `pos` into out, interleaved. Silence where a frame
 * does not decode. */
static void decode_samples(atrac *a, int32_t pos, int32_t n, int16_t *out) {
    const int32_t per = spf(a);
    int32_t done = 0;
    while (done < n) {
        const int32_t s = pos + done + a->skip, f = s / per, o = s % per;
        int32_t take = per - o;
        if (take > n - done) take = n - done;
        if (decode_frame(a, f) == 0)
            memcpy(out + done * a->channels, a->pcm + o * a->channels, (size_t)take * (size_t)a->channels * 2u);
        done += take;
    }
}
#endif

static atrac *get(uint32_t id) {
    return id < MAX_IDS && g_at[id].used ? &g_at[id] : NULL;
}

/* (u32 buf, u32 bufsize) -> id */
static void hle_SetDataAndGetID(void) {
    uint32_t buf = psp_arg(0), size = psp_arg(1);
    const uint8_t *d = (const uint8_t *)psp_mem_ptr(buf, size);
    int id = -1;
    for (int i = 0; i < MAX_IDS && id < 0; i++) if (!g_at[i].used) id = i;
    if (id < 0) { psp_ret(ATRAC_ERROR_NO_ATRACID); return; }
    atrac a;
    memset(&a, 0, sizeof a);
    if (!d || parse(&a, d, size) != 0) { psp_ret(ATRAC_ERROR_UNKNOWN_FORMAT); return; }
    a.used = 1;
    a.buf = buf;
    a.size = size;
    g_at[id] = a;
    printf("atrac: id %d, %s, %d ch, %u Hz, %d samples (skip %d), loop %d..%d, %u of %u data bytes in memory%s\n",
           id, a.plus ? "ATRAC3plus" : "ATRAC3", a.channels, a.rate, a.end_sample + 1, a.skip, a.loop_start,
           a.loop_end, size > a.data_off ? size - a.data_off : 0, a.data_size,
#if MHFU_FFMPEG
           ""
#else
           " (silent)"
#endif
           );
    psp_ret((uint32_t)id);
}

static void hle_ReleaseAtracID(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
#if MHFU_FFMPEG
    dec_close(a);
#endif
    a->used = 0;
    psp_ret(0);
}

/* (int id, u16 *out, int *samples, int *finish, int *remain) */
static void hle_DecodeData(void) {
    atrac *a = get(psp_arg(0));
    uint32_t out = psp_arg(1), samples_at = psp_arg(2), finish_at = psp_arg(3), remain_at = psp_arg(4);
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    int32_t end = a->loop_num != 0 && a->loop_end >= 0 ? a->loop_end : a->end_sample;
    if (a->pos > end) {
        if (samples_at) psp_write32(samples_at, 0);
        if (finish_at) psp_write32(finish_at, 1);
        if (remain_at) psp_write32(remain_at, (uint32_t)-1);
        psp_ret(ATRAC_ERROR_ALL_DATA_DECODED);
        return;
    }
    int32_t n = end - a->pos + 1;
    if (n > spf(a)) n = spf(a);
    uint32_t bytes = (uint32_t)n * (uint32_t)a->channels * 2u;
    void *p = out ? psp_mem_ptr(out, bytes) : NULL;
    if (p) memset(p, 0, bytes);
#if MHFU_FFMPEG
    if (p) decode_samples(a, a->pos, n, (int16_t *)p);
#endif
    a->pos += n;
    int finished = 0;
    if (a->pos > end) {
        if (a->loop_num != 0 && a->loop_start >= 0) {
            a->pos = a->loop_start;
            if (a->loop_num > 0) a->loop_num--;
        } else finished = 1;
    }
    if (samples_at) psp_write32(samples_at, (uint32_t)n);
    if (finish_at) psp_write32(finish_at, (uint32_t)finished);
    if (remain_at) psp_write32(remain_at, (uint32_t)-1);     /* the whole file is in memory */
    psp_ret(0);
}

/* (int id, int *remain): -1 = all the data is already loaded. */
static void hle_GetRemainFrame(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)-1);
    psp_ret(0);
}

/* (int id, int *endSample, int *loopStart, int *loopEnd) */
static void hle_GetSoundSample(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)a->end_sample);
    if (psp_arg(2)) psp_write32(psp_arg(2), (uint32_t)a->loop_start);
    if (psp_arg(3)) psp_write32(psp_arg(3), (uint32_t)a->loop_end);
    psp_ret(0);
}

/* (int id, u32 *kbps) */
static void hle_GetBitrate(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    uint32_t kbps = (uint32_t)((uint64_t)a->block * 8u * a->rate / (uint32_t)spf(a) / 1000u);
    if (psp_arg(1)) psp_write32(psp_arg(1), kbps);
    psp_ret(0);
}

static void hle_SetLoopNum(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    if (a->loop_start < 0) { psp_ret(ATRAC_ERROR_NO_LOOP_INFORMATION); return; }
    a->loop_num = (int)psp_arg(1);
    psp_ret(0);
}

/* (int id, int *loopNum, u32 *loopStatus) */
static void hle_GetLoopStatus(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)a->loop_num);
    if (psp_arg(2)) psp_write32(psp_arg(2), a->loop_start >= 0 ? 1u : 0u);
    psp_ret(0);
}

/* (int id, u32 *pos) */
static void hle_GetNextDecodePosition(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    if (a->pos > a->end_sample) { psp_ret(ATRAC_ERROR_ALL_DATA_DECODED); return; }
    if (psp_arg(1)) psp_write32(psp_arg(1), (uint32_t)a->pos);
    psp_ret(0);
}

/* (int id, u32 *writePtr, u32 *writableBytes, u32 *readOffset): nothing to
 * stream, the file is whole. */
static void hle_GetStreamDataInfo(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
    if (psp_arg(1)) psp_write32(psp_arg(1), a->buf);
    if (psp_arg(2)) psp_write32(psp_arg(2), 0);
    if (psp_arg(3)) psp_write32(psp_arg(3), a->size);
    psp_ret(0);
}

void mhfu_atrac_register(void) {
    memset(g_at, 0, sizeof g_at);
    psp_hle_register(0x7A20E7AF, "sceAtrac3plus", "sceAtracSetDataAndGetID", hle_SetDataAndGetID);
    psp_hle_register(0x61EB33F5, "sceAtrac3plus", "sceAtracReleaseAtracID", hle_ReleaseAtracID);
    psp_hle_register(0x6A8C3CD5, "sceAtrac3plus", "sceAtracDecodeData", hle_DecodeData);
    psp_hle_register(0x9AE849A7, "sceAtrac3plus", "sceAtracGetRemainFrame", hle_GetRemainFrame);
    psp_hle_register(0xA2BBA8BE, "sceAtrac3plus", "sceAtracGetSoundSample", hle_GetSoundSample);
    psp_hle_register(0xA554A158, "sceAtrac3plus", "sceAtracGetBitrate", hle_GetBitrate);
    psp_hle_register(0x868120B5, "sceAtrac3plus", "sceAtracSetLoopNum", hle_SetLoopNum);
    psp_hle_register(0xFAA4F89B, "sceAtrac3plus", "sceAtracGetLoopStatus", hle_GetLoopStatus);
    psp_hle_register(0xE23E3A35, "sceAtrac3plus", "sceAtracGetNextDecodePosition", hle_GetNextDecodePosition);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtracGetStreamDataInfo", hle_GetStreamDataInfo);
}
