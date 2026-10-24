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
 * Structures, sizes and error codes follow PPSSPP's Core/HLE/sceMpeg.cpp. A
 * real decoder (FFmpeg) would replace avc_step and write the frame. */
#include "mpeg.h"

#include <stdio.h>
#include <string.h>

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

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
        c->analyzed = 1;
        printf("mpeg: PSMF stream 0x%X bytes at 0x%X, %u frames (no decoder: playing as black)\n",
               size, offset, c->frames);
    }
    return offset;
}

/* packetsAvail is the packets *filled* (PPSSPP: packets minus the media
 * engine's free space); AvailableSize reports packets - packetsAvail free. */
static void update_avail(mpeg_ctx *c) {
    if (!c->ring || !c->analyzed) return;
    wr(c->ring + RB_AVAIL, (c->queued + PACKET - 1) / PACKET);
}

/* The video is over once the stream has been read in and used up, or the
 * frame count from the header has been shown. */
static int video_end(const mpeg_ctx *c) {
    if (!c->analyzed) return 0;
    if (c->video_frame >= c->frames) return 1;
    return c->total_put >= c->stream_size && c->queued == 0;
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
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->handle = h;
    c->ring = ring;
    psp_ret(0);
}

static void hle_Delete(void) {
    mpeg_ctx *c = ctx_of(psp_arg(0));
    if (c) c->used = 0;
    psp_ret(c ? 0 : 0xFFFFFFFFu);
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
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode", hle_AtracDecode);
}
