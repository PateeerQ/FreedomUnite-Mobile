/* MHFU-Android -- sceAtrac3plus with silent decoding.
 *
 * The game hands a whole ATRAC3/ATRAC3plus file (RIFF WAVE) to
 * SetDataAndGetID and then pulls PCM from DecodeData. Real decoding is not
 * here yet (FFmpeg, or a port of PPSSPP's decoder, can replace decode_frame).
 * Everything else is real: the header is parsed for channels, frame size,
 * sample count and loop points, and positions, loops and the end flag advance
 * as they would, so the game's music code runs its normal course and plays
 * silence. Unimplemented, these returned 0 without writing their outputs,
 * and the village went on with garbage (a return to a data address).
 *
 * Signatures and error codes follow PPSSPP's Core/HLE/sceAtrac.cpp. */
#include "atrac.h"

#include <stdio.h>
#include <string.h>

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

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
            have_fmt = 1;
        } else if (!memcmp(d + off, "fact", 4) && len >= 4 && off + 12 <= n) {
            a->end_sample = (int32_t)rd32(c) - 1;
            have_fact = 1;
        } else if (!memcmp(d + off, "smpl", 4) && len >= 36 + 24 && off + 8 + 60 <= n) {
            if (rd32(c + 28) > 0) {               /* loop count */
                a->loop_start = (int32_t)rd32(c + 36 + 8);
                a->loop_end = (int32_t)rd32(c + 36 + 12);
            }
        } else if (!memcmp(d + off, "data", 4)) {
            data_size = len;
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
    printf("atrac: id %d, %s, %d ch, %u Hz, %d samples, loop %d..%d (silent)\n", id,
           a.plus ? "ATRAC3plus" : "ATRAC3", a.channels, a.rate, a.end_sample + 1, a.loop_start, a.loop_end);
    psp_ret((uint32_t)id);
}

static void hle_ReleaseAtracID(void) {
    atrac *a = get(psp_arg(0));
    if (!a) { psp_ret(ATRAC_ERROR_BAD_ID); return; }
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
    if (p) memset(p, 0, bytes);                 /* decode_frame: silence for now */
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
