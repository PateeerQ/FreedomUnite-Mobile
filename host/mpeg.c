/* MHFU-Android -- sceMpeg without a video decoder.
 *
 * MHFU plays PMF movies (the intro and the title's attract loop) through
 * sceMpeg. There is no H.264 decoder here yet, so this does what PPSSPP's
 * sceMpeg does with its media engine reduced to bookkeeping: the ringbuffer is
 * real -- sceMpegRingbufferPut calls the game's callback, which reads the movie
 * file into it -- and each decoded frame consumes its share of the stream, so
 * the movie runs for its real length, shows nothing, and ends. The game then
 * carries on as it would after a movie.
 *
 * Before overlays could call firmware at all (see allegrexrecomp's import
 * registration), every one of these calls did nothing and the game skipped the
 * movie by accident. With the calls arriving and unimplemented, they returned
 * 0, "a frame decoded, more to come", forever: a black screen.
 *
 * Structures, sizes and error codes follow PPSSPP's Core/HLE/sceMpeg.cpp.
 *
 * With FFmpeg (MHFU_FFMPEG), the pictures are real, the way PPSSPP's media
 * engine does it: every packet the game puts into the ringbuffer is also
 * appended to a host-side copy of the stream, a small MPEG-PS demuxer here
 * takes the video PES packets out of it, FFmpeg's H.264 parser and decoder
 * turn them into pictures, and sceMpegAvcDecode writes the next one into the
 * game's buffer. The ringbuffer frees what the demuxer has used. (FFmpeg's own
 * PS demuxer cannot tell that a PSMF's video stream is H.264; PPSSPP also sets
 * that by hand.) The movie's ATRAC3+ sound is still silence. */
#include "mpeg.h"
#include "audio.h"
#include "hle_mhfu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#if MHFU_FFMPEG
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#endif

#define MPEG_ERROR_NO_DATA          0x80618001u
#define MPEG_ERROR_INVALID_VALUE    0x806101FEu
#define MPEG_ERROR_AVC_DECODE_FATAL 0x80628002u
#define MPEG_ERROR_NO_MEMORY        0x80610022u

#define MPEG_MEMSIZE        0x10000u    /* library version 0105 */
#define MPEG_AVC_ES_SIZE    2048u
#define MPEG_ATRAC_ES_SIZE  2112u
#define MPEG_ATRAC_OUT_SIZE 8192u
#define PSMF_MAGIC          0x464D5350u /* "PSMF" */
#define PACKET              2048u
#define TS_STEP             3003u       /* 90 kHz ticks per frame at 29.97 fps */

/* SceMpegRingBuffer, as the game sees it (offsets in bytes). */
enum {
    RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_PACKETSIZE = 16,
    RB_DATA = 20, RB_CALLBACK = 24, RB_CBARG = 28, RB_UPPER = 32, RB_SEMA = 36, RB_MPEG = 40,
};

/* SceMpegAu: s64 pts, s64 dts, u32 esBuffer, u32 esSize. */
enum { AU_PTS = 0, AU_DTS = 8, AU_ES = 16, AU_ESSIZE = 20 };

#define MAX_CTX 4

typedef struct {
    int      used;
    uint32_t handle;            /* the value the game passes as `mpeg` */
    uint32_t ring;
    int      analyzed;
    uint32_t stream_size;       /* PSMF: bytes of stream after the header */
    uint32_t frames;            /* video length in frames, from the timestamps */
    uint32_t queued;            /* bytes put and not yet consumed */
    uint64_t total_put;
    uint32_t video_frame, audio_frame;
    uint32_t per_frame;         /* bytes one frame consumes */
    int      es[2];
    uint32_t header_off;        /* the PSMF header's length: the stream starts there */
    int      pixel_mode;        /* sceMpegAvcDecodeMode: 0-3, 565 to 8888 */
    uint32_t frame_width;       /* sceMpegCreate's: used when a decode passes 0 (PPSSPP) */
#if MHFU_FFMPEG
    /* The stream as put, from the file's start: [pos, len) not yet demuxed. */
    uint8_t *buf;
    size_t   len, cap, pos;
    AVCodecParserContext *parser;
    AVCodecContext  *dec;
    AVFrame         *frame;
    AVPacket        *pkt;
    struct SwsContext *sws;
    int      failed;            /* FFmpeg could not open it: fall back to bookkeeping */
    int      draining;          /* the stream is used up; the decoder is emptying */
    int      ended;             /* demuxer and decoder both drained */
    /* The sound: a second cursor over buf picks out the audio packets into
     * aes, ATRAC3+ frames each with an 8-byte header (0F D0, size). */
    size_t   apos;
    uint8_t *aes;
    size_t   alen, acap;
    AVCodecContext *adec;
    AVFrame  *aframe;
    AVPacket *apkt;
    int      afailed;
    /* Where each stream starts (the first PES PTS, 90 kHz). The sound may
     * start before the pictures -- 0.59 s in the title movie -- and both are
     * counted from their own first frame, so that much sound is dropped first
     * (askip, in ATRAC3+ frames), or it is heard late all the way through. */
    uint64_t vpts0, apts0;
    int      have_vpts0, have_apts0, askip_set, askip;
#endif
} mpeg_ctx;

static mpeg_ctx g_ctx[MAX_CTX];

static uint32_t rd(uint32_t a)               { return psp_read32(a); }
static void     wr(uint32_t a, uint32_t v)   { psp_write32(a, v); }
static void     wr64(uint32_t a, uint64_t v) { wr(a, (uint32_t)v); wr(a + 4, (uint32_t)(v >> 32)); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* The handle the game holds is a pointer to a word that holds mpegAddr; the
 * context is looked up by that word's address, as PPSSPP does. */
static mpeg_ctx *ctx_of(uint32_t mpeg) {
    if (!mpeg) return NULL;
    const uint32_t h = rd(mpeg);
    for (int i = 0; i < MAX_CTX; i++)
        if (g_ctx[i].used && g_ctx[i].handle == h) return &g_ctx[i];
    return NULL;
}

static mpeg_ctx *ctx_of_ring(uint32_t ring) {
    for (int i = 0; i < MAX_CTX; i++)
        if (g_ctx[i].used && g_ctx[i].ring == ring) return &g_ctx[i];
    return NULL;
}

/* Read a PSMF header: stream offset and size, and the length from the first
 * and last timestamps. Returns the stream offset, or 0 if not a PSMF. */
static uint32_t analyze(mpeg_ctx *c, uint32_t addr, uint32_t *size_out) {
    const uint8_t *h = (const uint8_t *)psp_mem_ptr(addr, 0x60);
    if (!h) return 0;
    if (((uint32_t)h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24) != PSMF_MAGIC) return 0;
    const uint32_t offset = be32(h + 0x08), size = be32(h + 0x0C);
    if (size_out) *size_out = size;
    if (c && !c->analyzed) {
        const uint32_t first = be32(h + 0x56), last = be32(h + 0x5C);
        c->stream_size = size;
        c->frames = last > first ? (last - first) / TS_STEP + 1 : 1;
        c->per_frame = size / c->frames;
        if (c->per_frame < 512) c->per_frame = 512;
        c->header_off = offset;
        c->analyzed = 1;
        printf("mpeg: PSMF stream 0x%X bytes at 0x%X, %u frames (%s)\n", size, offset, c->frames,
#if MHFU_FFMPEG
               "FFmpeg"
#else
               "no decoder: playing as black"
#endif
               );
    }
    return offset;
}

/* packetsAvail is the packets *filled* (PPSSPP: packets minus the media
 * engine's free space); AvailableSize reports packets - packetsAvail free. */
static void update_avail(mpeg_ctx *c) {
    if (!c->ring || !c->analyzed) return;
    wr(c->ring + RB_AVAIL, (c->queued + PACKET - 1) / PACKET);
}

/* Everything of the file has been put: the header and the whole stream. */
static int all_put(const mpeg_ctx *c) {
    return c->analyzed && c->total_put >= (uint64_t)c->header_off + c->stream_size;
}

#if MHFU_FFMPEG
/* ---- the decoder: FFmpeg over a copy of what was put ----------------------- */

static int engine_live(const mpeg_ctx *c) { return c->dec && !c->failed; }

static void stream_append(mpeg_ctx *c, const uint8_t *p, size_t n) {
    /* Sound that lags far behind the pictures (a game that never asks for
     * it) is given up on, so the copy does not grow without end. */
    if (c->dec && c->pos > c->apos + ((size_t)2 << 20)) c->apos = c->pos;
    const size_t lo = c->dec && c->apos < c->pos ? c->apos : c->pos;
    if (lo > ((size_t)1 << 20) && lo * 2 > c->len) {           /* drop what both cursors passed */
        memmove(c->buf, c->buf + lo, c->len - lo);
        c->len -= lo;
        c->pos -= lo;
        c->apos = c->apos >= lo ? c->apos - lo : 0;
    }
    if (c->len + n > c->cap) {
        size_t cap = c->cap ? c->cap : (size_t)1 << 20;
        while (cap < c->len + n) cap *= 2;
        uint8_t *b = (uint8_t *)realloc(c->buf, cap);
        if (!b) return;
        c->buf = b;
        c->cap = cap;
    }
    memcpy(c->buf + c->len, p, n);
    c->len += n;
}

static void engine_close(mpeg_ctx *c) {
    if (c->parser) av_parser_close(c->parser);
    c->parser = NULL;
    avcodec_free_context(&c->dec);
    av_frame_free(&c->frame);
    av_packet_free(&c->pkt);
    sws_freeContext(c->sws);
    c->sws = NULL;
    free(c->buf);
    c->buf = NULL;
    c->len = c->cap = c->pos = 0;
    c->draining = c->ended = 0;
    avcodec_free_context(&c->adec);
    av_frame_free(&c->aframe);
    av_packet_free(&c->apkt);
    free(c->aes);
    c->aes = NULL;
    c->alen = c->acap = c->apos = 0;
    c->afailed = 0;
}

static int engine_fail(mpeg_ctx *c, const char *what) {
    fprintf(stderr, "mpeg: FFmpeg could not %s; the movie plays black\n", what);
    c->failed = 1;
    return 0;
}

/* The H.264 parser and decoder, once the header is in. 1 when open. */
static int engine_open(mpeg_ctx *c) {
    if (c->failed) return 0;
    if (c->dec) return 1;
    if (!c->analyzed || c->len < c->header_off) return 0;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return engine_fail(c, "find an H.264 decoder");
    c->parser = av_parser_init(AV_CODEC_ID_H264);
    c->dec = avcodec_alloc_context3(codec);
    if (!c->parser || !c->dec || avcodec_open2(c->dec, codec, NULL) < 0) return engine_fail(c, "open the decoder");
    c->frame = av_frame_alloc();
    c->pkt = av_packet_alloc();
    c->pos = c->apos = c->header_off;         /* the PSMF header is not MPEG-PS */
    c->queued = (uint32_t)(c->len - c->pos);
    return 1;
}

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* The next payload of one kind out of the program stream, from *cursor:
 * video (PES 0xE0-0xEF), or audio (private stream 1, 0xBD, less the 4 bytes
 * PSMF puts before the ATRAC3+ data). NULL when the next packet is not all
 * in yet; *n is 0 at the end of the stream. A PSMF is 2048-byte packs, so a
 * PES never runs more than a pack past what was put. */
static const uint8_t *demux_next(mpeg_ctx *c, size_t *cursor, int audio, size_t *n) {
    for (;;) {
        const size_t have = c->len - *cursor;
        const uint8_t *p = c->buf + *cursor;
        if (have < 6) { *n = 0; return all_put(c) ? p : NULL; }
        if (p[0] != 0 || p[1] != 0 || p[2] != 1) { (*cursor)++; continue; }   /* resync */
        const uint8_t id = p[3];
        if (id == 0xBA) {                                     /* pack header (MPEG-2) */
            if (have < 14) { *n = 0; return all_put(c) ? p : NULL; }
            *cursor += 14 + (p[13] & 7);
            continue;
        }
        if (id == 0xB9) { *cursor += 4; continue; }          /* end code */
        const size_t total = 6 + be16(p + 4);
        if (have < total) { *n = 0; return all_put(c) ? p : NULL; }
        *cursor += total;
        const int wanted = audio ? id == 0xBD : id >= 0xE0 && id <= 0xEF;
        if (!wanted || total < 9) continue;
        if ((p[7] & 0x80) && total >= 14 && !(audio ? c->have_apts0 : c->have_vpts0)) {
            const uint64_t pts = (uint64_t)(p[9] & 0x0E) << 29 | (uint64_t)p[10] << 22 |
                                 (uint64_t)(p[11] & 0xFE) << 14 | (uint64_t)p[12] << 7 | p[13] >> 1;
            if (audio) { c->apts0 = pts; c->have_apts0 = 1; }
            else       { c->vpts0 = pts; c->have_vpts0 = 1; }
        }
        /* MHFU_TRACE_MPEG: each stream's first PTS (90 kHz) and the private
         * stream's substream ids -- whether sound and pictures start together
         * and whether there is more than one sound track. */
        if (getenv("MHFU_TRACE_MPEG") && (p[7] & 0x80) && total >= 14) {
            static int seen_video, seen_sub[256];
            const uint64_t pts = (uint64_t)(p[9] & 0x0E) << 29 | (uint64_t)p[10] << 22 |
                                 (uint64_t)(p[11] & 0xFE) << 14 | (uint64_t)p[12] << 7 | p[13] >> 1;
            const int sub = audio && 9 + p[8] < total ? p[9 + p[8]] : -1;
            if (!audio && !seen_video) { seen_video = 1; fprintf(stderr, "mpeg: first video PES 0x%02X pts %llu (%.3f s)\n", id, (unsigned long long)pts, pts / 90000.0); }
            if (audio && sub >= 0 && !seen_sub[sub]) {
                seen_sub[sub] = 1;
                fprintf(stderr, "mpeg: first audio PES substream 0x%02X pts %llu (%.3f s)\n", sub, (unsigned long long)pts, pts / 90000.0);
            }
        }
        const size_t start = 9 + p[8] + (audio ? 4 : 0);
        if (start > total) continue;
        *n = total - start;
        return p + start;
    }
}

/* Decode the next picture into c->frame: 1 for a picture, 0 for none yet
 * (more of the stream is needed), -1 at the end. */
static int engine_frame(mpeg_ctx *c) {
    for (;;) {
        int r = avcodec_receive_frame(c->dec, c->frame);
        if (r == 0) return 1;
        if (r == AVERROR_EOF) { c->ended = 1; return -1; }
        if (c->draining) { c->ended = 1; return -1; }
        size_t n = 0;
        const uint8_t *es = demux_next(c, &c->pos, 0, &n);
        c->queued = (uint32_t)(c->len - c->pos);              /* the ringbuffer frees it */
        if (!es) return 0;                                    /* wait for more */
        if (!n) {                                             /* the end: flush everything */
            uint8_t *out; int out_n;
            av_parser_parse2(c->parser, c->dec, &out, &out_n, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (out_n) {
                c->pkt->data = out; c->pkt->size = out_n;
                avcodec_send_packet(c->dec, c->pkt);
            }
            avcodec_send_packet(c->dec, NULL);
            c->draining = 1;
            r = avcodec_receive_frame(c->dec, c->frame);
            if (r == 0) return 1;
            c->ended = 1;
            return -1;
        }
        if (getenv("MHFU_DUMP_MPEG")) {                       /* the elementary stream, for ffmpeg -i */
            static FILE *dump;
            if (!dump) dump = fopen(getenv("MHFU_DUMP_MPEG"), "wb");
            if (dump) fwrite(es, 1, n, dump);
        }
        while (n > 0) {
            uint8_t *out; int out_n;
            const int used = av_parser_parse2(c->parser, c->dec, &out, &out_n, es, (int)n,
                                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) break;
            es += used; n -= (size_t)used;
            if (out_n) {
                c->pkt->data = out; c->pkt->size = out_n;
                avcodec_send_packet(c->dec, c->pkt);
            }
        }
    }
}

/* The picture into the game's buffer, `width` pixels a row, in its pixel
 * format: 0-3 are 565, 5551, 4444, 8888, red in the low bits. */
static void engine_write(mpeg_ctx *c, uint32_t dst, uint32_t width) {
    static const enum AVPixelFormat PF[4] = { AV_PIX_FMT_BGR565LE, AV_PIX_FMT_BGR555LE,
                                              AV_PIX_FMT_BGR444LE, AV_PIX_FMT_RGBA };
    const int mode = c->pixel_mode & 3, bpp = mode == 3 ? 4 : 2;
    const int w = c->frame->width, h = c->frame->height;
    if (!width || (int)width < w) width = (uint32_t)w;
    uint8_t *p = (uint8_t *)psp_mem_ptr(dst, width * (uint32_t)h * (uint32_t)bpp);
    if (!p) return;
    c->sws = sws_getCachedContext(c->sws, w, h, (enum AVPixelFormat)c->frame->format, w, h, PF[mode],
                                  SWS_POINT, NULL, NULL, NULL);
    if (!c->sws) return;
    uint8_t *planes[4] = { p, NULL, NULL, NULL };
    int strides[4] = { (int)width * bpp, 0, 0, 0 };
    sws_scale(c->sws, (const uint8_t *const *)c->frame->data, c->frame->linesize, 0, h, planes, strides);
}

/* ---- the movie's sound: ATRAC3+ frames out of the audio packets ----------- */

static void aes_append(mpeg_ctx *c, const uint8_t *p, size_t n) {
    if (c->alen + n > c->acap) {
        size_t cap = c->acap ? c->acap : 16384;
        while (cap < c->alen + n) cap *= 2;
        uint8_t *b = (uint8_t *)realloc(c->aes, cap);
        if (!b) return;
        c->aes = b;
        c->acap = cap;
    }
    memcpy(c->aes + c->alen, p, n);
    c->alen += n;
}

static int audio_open(mpeg_ctx *c, int block_align) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    c->adec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (c->adec) {
        c->adec->sample_rate = 44100;
        av_channel_layout_default(&c->adec->ch_layout, 2);
        c->adec->block_align = block_align;
    }
    if (!c->adec || avcodec_open2(c->adec, codec, NULL) < 0) {
        fprintf(stderr, "mpeg: no ATRAC3+ decoder; the movie is silent\n");
        avcodec_free_context(&c->adec);
        c->afailed = 1;
        return 0;
    }
    c->aframe = av_frame_alloc();
    c->apkt = av_packet_alloc();
    return 1;
}

/* The next frame's samples into out as interleaved stereo 16-bit; returns the
 * samples written, 0 when there is no frame (yet). Each frame is an 8-byte
 * header -- 0F D0, then in the low 10 bits of the next two bytes n, where the
 * frame is n * 8 + 16 bytes header included (PPSSPP's MpegDemux) -- and the
 * ATRAC3+ data. */
static int audio_frame(mpeg_ctx *c, int16_t *out, int max_samples) {
    for (;;) {
        if (c->alen >= 8 && c->aes[0] == 0x0F && c->aes[1] == 0xD0) {
            const size_t fs = ((((size_t)c->aes[2] & 3) << 8) | c->aes[3]) * 8 + 16;
            if (c->alen >= fs) {
                if (!c->adec && !audio_open(c, (int)fs - 8)) return 0;
                c->apkt->data = c->aes + 8;
                c->apkt->size = (int)fs - 8;
                const int ok = avcodec_send_packet(c->adec, c->apkt) >= 0 &&
                               avcodec_receive_frame(c->adec, c->aframe) >= 0;
                memmove(c->aes, c->aes + fs, c->alen - fs);
                c->alen -= fs;
                if (!ok) continue;
                /* Line the sound up with the pictures: drop what plays before
                 * the first picture, plus the output's own latency (the rings,
                 * psprecomp's lead, the device, and how far ahead of the
                 * screen the game decodes its pictures). 1250 ms, set by ear
                 * on Windows against the title trailer (93 ms left the sound
                 * clearly late); MHFU_MOVIE_SYNC_MS sets it. */
                if (!c->askip_set && c->have_apts0 && c->have_vpts0) {
                    c->askip_set = 1;
                    const char *env = getenv("MHFU_MOVIE_SYNC_MS");
                    const double out_s = (env ? atof(env) : 1250.0) / 1000.0;
                    const double lead_s = c->vpts0 > c->apts0 ? (double)(c->vpts0 - c->apts0) / 90000.0 : 0.0;
                    const double skip_s = lead_s + out_s;
                    c->askip = skip_s > 0 ? (int)(skip_s * 44100.0 / 2048.0 + 0.5) : 0;
                    printf("mpeg: sound starts %.3f s before the pictures, output latency %.0f ms;"
                           " dropping its first %d frames\n", lead_s, out_s * 1000.0, c->askip);
                }
                if (c->askip > 0) { c->askip--; continue; }
                const AVFrame *f = c->aframe;
                const int chans = f->ch_layout.nb_channels;
                const int n = f->nb_samples < max_samples ? f->nb_samples : max_samples;
                for (int i = 0; i < n; i++)
                    for (int ch = 0; ch < 2; ch++) {
                        const int src = chans > 1 ? ch : 0;
                        float v = f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->extended_data[src])[i]
                                : f->format == AV_SAMPLE_FMT_FLT ? ((const float *)f->data[0])[i * chans + src]
                                : 0.0f;
                        v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                        out[i * 2 + ch] = (int16_t)(v * 32767.0f);
                    }
                return n;
            }
        } else if (c->alen >= 2) {                            /* lost the sync: find it again */
            size_t i = 1;
            while (i + 1 < c->alen && !(c->aes[i] == 0x0F && c->aes[i + 1] == 0xD0)) i++;
            memmove(c->aes, c->aes + i, c->alen - i);
            c->alen -= i;
            if (c->alen >= 2 && c->aes[0] == 0x0F && c->aes[1] == 0xD0) continue;
        }
        size_t n = 0;
        const uint8_t *p = demux_next(c, &c->apos, 1, &n);
        if (!p || !n) return 0;
        aes_append(c, p, n);
    }
}
#endif

/* The video is over once the stream has been read in and used up, or the
 * frame count from the header has been shown. */
static int video_end(const mpeg_ctx *c) {
    if (!c->analyzed) return 0;
#if MHFU_FFMPEG
    if (engine_live(c)) return c->ended;
#endif
    if (c->video_frame >= c->frames) return 1;
    return all_put(c) && c->queued == 0;
}

static void hle_Init(void)   { memset(g_ctx, 0, sizeof g_ctx); psp_ret(0); }
static void hle_Finish(void) { psp_ret(0); }
static void hle_QueryMemSize(void) { psp_ret(MPEG_MEMSIZE); }

/* (int packets) */
static void hle_RingbufferQueryMemSize(void) { psp_ret(psp_arg(0) * (104 + PACKET)); }

/* (ring, packets, data, size, callback, cbarg) */
static void hle_RingbufferConstruct(void) {
    const uint32_t ring = psp_arg(0), packets = psp_arg(1), data = psp_arg(2);
    if (!psp_mem_ptr(ring, 44)) { psp_ret(0x80020323u); return; }
    if (packets * (104 + PACKET) > psp_arg(3) && packets < 0x00100000u) { psp_ret(MPEG_ERROR_NO_MEMORY); return; }
    wr(ring + RB_PACKETS, packets);
    wr(ring + RB_READ, 0);
    wr(ring + RB_WRITEPOS, 0);
    wr(ring + RB_AVAIL, 0);
    wr(ring + RB_PACKETSIZE, PACKET);
    wr(ring + RB_DATA, data);
    wr(ring + RB_CALLBACK, psp_arg(4));
    wr(ring + RB_CBARG, psp_arg(5));
    wr(ring + RB_UPPER, data + packets * PACKET);
    wr(ring + RB_MPEG, 0);
    psp_ret(0);
}

static void hle_RingbufferDestruct(void) { psp_ret(0); }

/* (mpegAddr, data, size, ring, frameWidth, mode, ddrTop) */
static void hle_Create(void) {
    const uint32_t mpeg = psp_arg(0), data = psp_arg(1), ring = psp_arg(3);
    if (!psp_mem_ptr(mpeg, 4)) { psp_ret(0xFFFFFFFFu); return; }
    if (psp_arg(2) < MPEG_MEMSIZE) { psp_ret(MPEG_ERROR_NO_MEMORY); return; }
    if (ring && psp_mem_ptr(ring, 44)) {
        const uint32_t ps = rd(ring + RB_PACKETSIZE);
        wr(ring + RB_AVAIL, ps ? rd(ring + RB_PACKETS) - (rd(ring + RB_UPPER) - rd(ring + RB_DATA)) / ps : 0);
        wr(ring + RB_MPEG, mpeg);
    }
    /* The handle: mpegAddr written at data + 0x30, which holds a fake
     * "LIBMPEG" header the game may inspect. */
    const uint32_t h = data + 0x30;
    wr(mpeg, h);
    psp_mem_write_block(h, "LIBMPEG\0" "001\0", 12);
    wr(h + 12, 0xFFFFFFFFu);
    wr(h + 16, ring);
    wr(h + 20, ring ? rd(ring + RB_UPPER) : 0);

    mpeg_ctx *c = NULL;
    for (int i = 0; i < MAX_CTX && !c; i++) if (g_ctx[i].used && g_ctx[i].handle == h) c = &g_ctx[i];
    for (int i = 0; i < MAX_CTX && !c; i++) if (!g_ctx[i].used) c = &g_ctx[i];
    if (!c) { psp_ret(MPEG_ERROR_NO_MEMORY); return; }
#if MHFU_FFMPEG
    engine_close(c);
#endif
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->handle = h;
    c->ring = ring;
    c->pixel_mode = 3;              /* 8888 until sceMpegAvcDecodeMode says otherwise */
    c->frame_width = psp_arg(4);
    psp_ret(0);
}

static void hle_Delete(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (c) {
#if MHFU_FFMPEG
        engine_close(c);
#endif
        c->used = 0;
    }
    psp_ret(c ? 0 : 0xFFFFFFFFu);
}

/* (mpeg, *mode): SceMpegAvcMode { int unknown; int pixelMode; } */
static void hle_AvcDecodeMode(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const uint32_t m = psp_arg(1);
    if (!c || !psp_mem_ptr(m, 8)) { psp_ret(0xFFFFFFFFu); return; }
    const int32_t mode = (int32_t)rd(m + 4);
    if (mode >= 0 && mode <= 3) c->pixel_mode = mode;
    psp_ret(0);
}

/* (mpeg, buffer, *offset) */
static void hle_QueryStreamOffset(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    const uint32_t off = analyze(c, psp_arg(1), NULL);
    if (!off || (off & 2047)) { wr(psp_arg(2), 0); psp_ret(MPEG_ERROR_INVALID_VALUE); return; }
    wr(psp_arg(2), off);
    psp_ret(0);
}

/* (buffer, *size) */
static void hle_QueryStreamSize(void) {
    uint32_t size = 0;
    const uint32_t off = analyze(NULL, psp_arg(0), &size);
    if (!off || (off & 2047)) { wr(psp_arg(1), 0); psp_ret(MPEG_ERROR_INVALID_VALUE); return; }
    wr(psp_arg(1), size);
    psp_ret(0);
}

/* (mpeg, type, num): returns a stream handle; any nonzero value will do. */
static void hle_RegistStream(void) {
    static uint32_t next = 0x1000;
    psp_ret(ctx_of(psp_arg(0)) ? next++ : 0xFFFFFFFFu);
}
static void hle_UnRegistStream(void) { psp_ret(0); }
static void hle_FlushAllStream(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (c) {
#if MHFU_FFMPEG
        engine_close(c);
        c->failed = 0;
#endif
        c->queued = 0;
        if (c->ring) { wr(c->ring + RB_READ, 0); wr(c->ring + RB_WRITEPOS, 0); }
        update_avail(c);
    }
    psp_ret(0);
}

static void hle_MallocAvcEsBuf(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    for (int i = 0; i < 2; i++) if (!c->es[i]) { c->es[i] = 1; psp_ret((uint32_t)i + 1); return; }
    psp_ret(0);
}
static void hle_FreeAvcEsBuf(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const uint32_t b = psp_arg(1);
    if (c && b >= 1 && b <= 2) c->es[b - 1] = 0;
    psp_ret(0);
}

/* (mpeg, esBuffer, au) */
static void hle_InitAu(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const uint32_t b = psp_arg(1), au = psp_arg(2);
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    const int avc = b >= 1 && b <= 2 && c->es[b - 1];
    wr64(au + AU_PTS, 0);
    wr64(au + AU_DTS, avc ? 0 : 0xFFFFFFFFFFFFFFFFull);
    wr(au + AU_ES, 0);
    wr(au + AU_ESSIZE, avc ? MPEG_AVC_ES_SIZE : MPEG_ATRAC_ES_SIZE);
    psp_ret(0);
}

static void hle_QueryAtracEsSize(void) {
    if (psp_arg(1)) wr(psp_arg(1), MPEG_ATRAC_ES_SIZE);
    if (psp_arg(2)) wr(psp_arg(2), MPEG_ATRAC_OUT_SIZE);
    psp_ret(0);
}

/* (ring) -> free packets */
static void hle_RingbufferAvailableSize(void) {
    const uint32_t ring = psp_arg(0);
    mpeg_ctx *c = ctx_of_ring(ring);
    if (!c) {
        const uint32_t m = rd(ring + RB_MPEG);
        c = m ? ctx_of(m) : NULL;
        if (c) c->ring = ring;
    }
    if (!c) { psp_ret(0x80618009u); return; }   /* not yet init */
    update_avail(c);
    psp_ret(rd(ring + RB_PACKETS) - rd(ring + RB_AVAIL));
}

/* Run the game's read callback: (data, packets, arg) -> packets read. */
static int32_t put_callback(uint32_t fn, uint32_t data, uint32_t n, uint32_t arg) {
    if (!psp_lookup(fn)) return -1;
    psp_cpu_state saved = psp_cpu;
    psp_cpu.r[PSP_REG_SP] = (psp_cpu.r[PSP_REG_SP] - 64) & ~15u;
    psp_cpu.r[PSP_REG_A0] = data;
    psp_cpu.r[PSP_REG_A1] = n;
    psp_cpu.r[PSP_REG_A2] = arg;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_call_from_host(fn);
    const int32_t got = (int32_t)psp_cpu.r[PSP_REG_V0];
    psp_cpu = saved;
    return got;
}

/* (ring, packets, available) */
static void hle_RingbufferPut(void) {
    const uint32_t ring = psp_arg(0);
    int32_t want = (int32_t)psp_arg(1);
    const int32_t avail_arg = (int32_t)psp_arg(2);
    const int32_t packets = (int32_t)rd(ring + RB_PACKETS);
    if (want > avail_arg) want = avail_arg;
    if (want > packets - (int32_t)rd(ring + RB_AVAIL)) want = packets - (int32_t)rd(ring + RB_AVAIL);
    if (want <= 0 || packets <= 0) { psp_ret(0); return; }
    mpeg_ctx *c = ctx_of(rd(ring + RB_MPEG));
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    c->ring = ring;
    const uint32_t fn = rd(ring + RB_CALLBACK), arg = rd(ring + RB_CBARG), data = rd(ring + RB_DATA);
    int32_t total = 0;
    while (want > 0) {
        const int32_t wpos = (int32_t)rd(ring + RB_WRITEPOS) % packets;
        int32_t n = want < packets - wpos ? want : packets - wpos;
        const int32_t got = put_callback(fn, data + (uint32_t)wpos * PACKET, (uint32_t)n, arg);
        if (got <= 0) { if (!total && got < 0) total = got; break; }
        const int32_t added = got > n ? n : got;
        if (rd(ring + RB_READ) == 0 && !c->analyzed) analyze(c, data + (uint32_t)wpos * PACKET, NULL);
#if MHFU_FFMPEG
        if (!c->failed) {
            const uint8_t *src = (const uint8_t *)psp_mem_ptr(data + (uint32_t)wpos * PACKET, (uint32_t)added * PACKET);
            if (src) stream_append(c, src, (size_t)added * PACKET);
        }
#endif
        wr(ring + RB_READ, rd(ring + RB_READ) + (uint32_t)added);
        wr(ring + RB_WRITEPOS, rd(ring + RB_WRITEPOS) + (uint32_t)added);
        c->queued += (uint32_t)added * PACKET;
        c->total_put += (uint64_t)added * PACKET;
        total += added;
        want -= added;
        update_avail(c);
        if (added < n) break;          /* the file ran out */
    }
    psp_ret((uint32_t)total);
}

static int has_data(mpeg_ctx *c) {
#if MHFU_FFMPEG
    /* The demuxer holds read-ahead of its own: at the end of the file the
     * ringbuffer is empty with pictures still to come. */
    if (engine_live(c) && !c->ended && all_put(c)) return 1;
#endif
    return c->ring && rd(c->ring + RB_READ) != 0 && rd(c->ring + RB_AVAIL) != 0;
}

/* (mpeg, stream, au, *attr) */
static void hle_GetAvcAu(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const uint32_t au = psp_arg(2), attr = psp_arg(3);
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    update_avail(c);
    if (!has_data(c)) {
        wr64(au + AU_PTS, 0); wr64(au + AU_DTS, 0);
        psp_ret(MPEG_ERROR_NO_DATA);
        return;
    }
    const uint64_t pts = 90000 + (uint64_t)c->video_frame * TS_STEP;
    wr64(au + AU_PTS, pts);
    if (video_end(c)) {
        wr64(au + AU_DTS, 0xFFFFFFFFFFFFFFFFull);
        psp_ret(MPEG_ERROR_NO_DATA);
        return;
    }
    wr64(au + AU_DTS, pts - TS_STEP);
    if (attr && psp_mem_ptr(attr, 4)) wr(attr, 1);
    psp_ret(0);
}

/* (mpeg, stream, au, *attr) */
static void hle_GetAtracAu(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const uint32_t au = psp_arg(2), attr = psp_arg(3);
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    update_avail(c);
    if (!has_data(c) || video_end(c)) {
        wr64(au + AU_PTS, 0);
        psp_ret(MPEG_ERROR_NO_DATA);
        return;
    }
    wr64(au + AU_PTS, 90000 + (uint64_t)c->audio_frame * 4180);   /* 2048 samples at 44.1 kHz */
    wr64(au + AU_DTS, 0xFFFFFFFFFFFFFFFFull);
    if (attr && psp_mem_ptr(attr, 4)) wr(attr, 0);
    psp_ret(0);
}

/* (mpeg, au, frameWidth, *buffer, *init): one frame's worth of the stream is
 * used up and the frame left as it was. */
static void hle_AvcDecode(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    const uint32_t init = psp_arg(4);
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    if (!c->ring || rd(c->ring + RB_READ) == 0 || video_end(c)) { psp_ret(MPEG_ERROR_AVC_DECODE_FATAL); return; }
#if MHFU_FFMPEG
    if (engine_open(c)) {
        const int got = engine_frame(c);
        const uint32_t at = psp_arg(3) ? rd(psp_arg(3)) : 0;
        if (got > 0) {
            if (at) engine_write(c, at, psp_arg(2) ? psp_arg(2) : c->frame_width);
            c->video_frame++;
        }
        static unsigned calls;
        if (getenv("MHFU_TRACE_MPEG") && calls == 300) psp_ge_dump_next(4);   /* what draws the movie */
        if (getenv("MHFU_TRACE_MPEG") && calls++ % 61 < 2) {
            uint32_t da, ds, df;
            psp_display_get(&da, &ds, &df);
            fprintf(stderr, "mpeg: decode #%u got %d frames %u pos 0x%zX len 0x%zX put 0x%llX to 0x%08X width %u mode %d"
                    " display 0x%08X/%u fmt %u\n",
                    calls, got, c->video_frame, c->pos, c->len, (unsigned long long)c->total_put, at, psp_arg(2),
                    c->pixel_mode, da, ds, df);
        }
        update_avail(c);
        if (init && psp_mem_ptr(init, 4)) wr(init, got > 0 ? 1 : 0);
        psp_ret(0);
        return;
    }
    if (!c->failed) {                  /* still buffering before the open */
        if (init && psp_mem_ptr(init, 4)) wr(init, 0);
        psp_ret(0);
        return;
    }
#endif
    const uint32_t use = c->per_frame < c->queued ? c->per_frame : c->queued;
    c->queued -= use;
    c->video_frame++;
    update_avail(c);
    if (init && psp_mem_ptr(init, 4)) wr(init, 1);
    psp_ret(0);
}

/* (mpeg, frameWidth, buffer, *status) */
static void hle_AvcDecodeStop(void) {
    if (psp_arg(3) && psp_mem_ptr(psp_arg(3), 4)) wr(psp_arg(3), 0);
    psp_ret(0);
}

/* (mpeg, au, buffer, init): silence. */
static void hle_AtracDecode(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (!c) { psp_ret(0xFFFFFFFFu); return; }
    void *out = psp_mem_ptr(psp_arg(2), MPEG_ATRAC_OUT_SIZE);
    if (out) memset(out, 0, MPEG_ATRAC_OUT_SIZE);
#if MHFU_FFMPEG
    static unsigned all;
    if (getenv("MHFU_TRACE_MPEG") && all++ % 100 == 0)
        fprintf(stderr, "mpeg: at %.2f s: video %u frames (%.2f s of movie), audio %u frames (%.2f s)\n",
                (double)clock() / CLOCKS_PER_SEC, c->video_frame, c->video_frame * 3003 / 90000.0,
                c->audio_frame, c->audio_frame * 2048 / 44100.0);
    /* 2048 stereo samples, or silence while there is no frame. */
    if (out && engine_live(c) && !c->afailed) {
        const int n = audio_frame(c, (int16_t *)out, MPEG_ATRAC_OUT_SIZE / 4);
        if (getenv("MHFU_TRACE_MPEG")) {
            static int marked;
            for (int i = 0; i < n * 2 && !marked; i++) {
                const int v = ((const int16_t *)out)[i];
                if (v > 64 || v < -64) {
                    marked = 1;
                    fprintf(stderr, "mpeg: first audible audio at decode #%u (video frame %u)\n",
                            c->audio_frame, c->video_frame);
                    mhfu_audio_mark(mhfu_clock_us());
                }
            }
        }
        static unsigned calls;
        if (getenv("MHFU_TRACE_MPEG") && (calls++ % 100 == 0 || calls < 4)) {
            int peak = 0;
            for (int i = 0; i < n * 2; i++) {
                const int v = ((const int16_t *)out)[i];
                if (v > peak) peak = v; else if (-v > peak) peak = -v;
            }
            fprintf(stderr, "mpeg: audio #%u %d samples, peak %d\n", calls, n, peak);
        }
    }
#endif
    c->audio_frame++;
    psp_ret(0);
}

void mhfu_mpeg_register(void) {
    memset(g_ctx, 0, sizeof g_ctx);
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit", hle_Init);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish", hle_Finish);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize", hle_QueryMemSize);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize", hle_RingbufferQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct", hle_RingbufferConstruct);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct", hle_RingbufferDestruct);
    psp_hle_register(0xD8C5F121, "sceMpeg", "sceMpegCreate", hle_Create);
    psp_hle_register(0x606A4649, "sceMpeg", "sceMpegDelete", hle_Delete);
    psp_hle_register(0x21FF80E4, "sceMpeg", "sceMpegQueryStreamOffset", hle_QueryStreamOffset);
    psp_hle_register(0x611E9E11, "sceMpeg", "sceMpegQueryStreamSize", hle_QueryStreamSize);
    psp_hle_register(0x42560F23, "sceMpeg", "sceMpegRegistStream", hle_RegistStream);
    psp_hle_register(0x591A4AA2, "sceMpeg", "sceMpegUnRegistStream", hle_UnRegistStream);
    psp_hle_register(0x707B7629, "sceMpeg", "sceMpegFlushAllStream", hle_FlushAllStream);
    psp_hle_register(0xA780CF7E, "sceMpeg", "sceMpegMallocAvcEsBuf", hle_MallocAvcEsBuf);
    psp_hle_register(0xCEB870B1, "sceMpeg", "sceMpegFreeAvcEsBuf", hle_FreeAvcEsBuf);
    psp_hle_register(0x167AFD9E, "sceMpeg", "sceMpegInitAu", hle_InitAu);
    psp_hle_register(0xF8DCB679, "sceMpeg", "sceMpegQueryAtracEsSize", hle_QueryAtracEsSize);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize", hle_RingbufferAvailableSize);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut", hle_RingbufferPut);
    psp_hle_register(0xFE246728, "sceMpeg", "sceMpegGetAvcAu", hle_GetAvcAu);
    psp_hle_register(0xE1CE83A7, "sceMpeg", "sceMpegGetAtracAu", hle_GetAtracAu);
    psp_hle_register(0x0E3C2E9D, "sceMpeg", "sceMpegAvcDecode", hle_AvcDecode);
    psp_hle_register(0x740FCCD1, "sceMpeg", "sceMpegAvcDecodeStop", hle_AvcDecodeStop);
    psp_hle_register(0xA11C7026, "sceMpeg", "sceMpegAvcDecodeMode", hle_AvcDecodeMode);
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode", hle_AtracDecode);
}
