/* MHFU-Android -- see audio.h.
 *
 * Each PSP output channel feeds a ring of stereo 16-bit frames. The output --
 * SDL3 (WASAPI on Windows, AAudio on Android) when it is linked, else WinMM --
 * pulls from all of them at 44.1 kHz, mixed. A
 * channel that falls behind plays silence; one that runs ahead loses its
 * oldest frames, so latency stays bounded (the guest is paced by psprecomp's
 * blocking output, not by this). */
#include "audio.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "psprecomp/hle.h"
#include "hle_mhfu.h"
#include <stdio.h>

#define CHANNELS   8
#define RING       16384          /* frames per channel, about 0.37 s */
#define MAX_AHEAD  (4096 + 2048)  /* frames a channel may queue before dropping */

typedef struct {
    int16_t buf[RING * 2];
    volatile uint32_t rd, wr;     /* frame counters; the ring is wr - rd frames full */
} ring;

static ring g_ring[CHANNELS];

/* For the log: frames the game sent, frames the output wanted but a playing
 * channel did not have (heard as gaps), frames dropped for running ahead. */
static volatile uint64_t g_in_frames, g_short_frames, g_dropped_frames;

/* Latency, for the log: the fullest ring as the output reads it, summed over
 * reads, plus what the output device still holds (set by the SDL path). */
static volatile uint64_t g_lat_sum, g_lat_n, g_dev_frames;

void mhfu_audio_stats(uint64_t *in, uint64_t *short_, uint64_t *dropped) {
    *in = g_in_frames; *short_ = g_short_frames; *dropped = g_dropped_frames;
}

double mhfu_audio_latency_ms(void) {
    const uint64_t n = g_lat_n, sum = g_lat_sum;
    g_lat_n = 0;
    g_lat_sum = 0;
    const double ring = n ? (double)sum / (double)n : 0.0;
    return (ring + (double)g_dev_frames) * 1000.0 / 44100.0;
}

/* mhfu_audio_mark: the time a decoder produced its first sound, so the first
 * sound to reach the output after it can say how long the game held it. */
static volatile uint64_t g_mark_us;
void mhfu_audio_mark(uint64_t decoded_us) { if (!g_mark_us) g_mark_us = decoded_us; }

/* psprecomp's sceAudio hands every buffer here (patch 0014). */
static void sink(int ch, const int16_t *pcm, uint32_t frames, int stereo, int vol_l, int vol_r) {
    if (ch < 0 || ch >= CHANNELS || !pcm) return;
    if (g_mark_us) {
        for (uint32_t i = 0; i < frames * (stereo ? 2 : 1); i++) {
            if (pcm[i] > 64 || pcm[i] < -64) {
                printf("audio: the first decoded sound reached sceAudio %.0f ms after it was decoded\n",
                       (double)(mhfu_clock_us() - g_mark_us) / 1000.0);
                g_mark_us = 0;
                break;
            }
        }
    }
    ring *r = &g_ring[ch];
    uint32_t wr = r->wr;
    if (wr - r->rd > MAX_AHEAD) {                         /* too far ahead: drop the oldest */
        g_dropped_frames += wr - r->rd - MAX_AHEAD;
        r->rd = wr - MAX_AHEAD;
    }
    g_in_frames += frames;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = stereo ? pcm[2 * i] : pcm[i];
        int32_t rr = stereo ? pcm[2 * i + 1] : pcm[i];
        uint32_t at = (wr + i) % RING;
        r->buf[2 * at] = (int16_t)(l * vol_l / 0x8000);
        r->buf[2 * at + 1] = (int16_t)(rr * vol_r / 0x8000);
    }
    r->wr = wr + frames;
}

static void mix(int16_t *out, uint32_t frames) {
    static int32_t acc[2 * 2048];
    memset(acc, 0, (size_t)frames * 2 * sizeof acc[0]);
    uint32_t fullest = 0;
    for (int c = 0; c < CHANNELS; c++) {
        ring *r = &g_ring[c];
        uint32_t have = r->wr - r->rd, n = have < frames ? have : frames;
        if (have > fullest) fullest = have;
        /* A channel that has played and now runs short leaves a gap. */
        if (r->wr && n < frames) g_short_frames += frames - n;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = (r->rd + i) % RING;
            acc[2 * i] += r->buf[2 * at];
            acc[2 * i + 1] += r->buf[2 * at + 1];
        }
        r->rd += n;
    }
    g_lat_sum += fullest;
    g_lat_n++;
    for (uint32_t i = 0; i < frames * 2; i++)
        out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}

#if MHFU_GAMEPAD   /* SDL3 is linked: play through it */

#include <SDL3/SDL.h>
#include <stdio.h>

/* SDL asks for bytes as its device drains; they are mixed then. */
static void SDLCALL pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)user; (void)total;
    static int16_t pcm[2048 * 2];
    while (additional > 0) {
        uint32_t frames = (uint32_t)additional / 4;
        if (frames == 0) break;
        if (frames > 2048) frames = 2048;
        mix(pcm, frames);
        SDL_PutAudioStreamData(stream, pcm, (int)frames * 4);
        additional -= (int)frames * 4;
    }
    g_dev_frames = (uint64_t)(SDL_GetAudioStreamQueued(stream) / 4);
}

int mhfu_audio_start(void) {
    /* A short device buffer: 256 frames, under 6 ms. */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "audio: SDL: %s\n", SDL_GetError());
        return -1;
    }
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, NULL);
    if (!s) {
        fprintf(stderr, "audio: SDL: %s\n", SDL_GetError());
        return -1;
    }
    psp_audio_set_sink(sink);
    SDL_ResumeAudioStreamDevice(s);
    return 0;
}

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#define NBUF    4
#define FRAMES  512               /* per buffer: 11.6 ms; the four are the output latency */

static HWAVEOUT g_wo;
static WAVEHDR  g_hdr[NBUF];
static int16_t  g_pcm[NBUF][FRAMES * 2];

static DWORD WINAPI feeder(LPVOID ev) {
    for (;;) {
        WaitForSingleObject((HANDLE)ev, 50);
        for (int i = 0; i < NBUF; i++) {
            if (!(g_hdr[i].dwFlags & WHDR_DONE)) continue;
            mix(g_pcm[i], FRAMES);
            g_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        }
    }
    return 0;
}

int mhfu_audio_start(void) {
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = 44100;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = 44100 * 4;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ev || waveOutOpen(&g_wo, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return -1;
    for (int i = 0; i < NBUF; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_pcm[i];
        g_hdr[i].dwBufferLength = sizeof g_pcm[i];
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].dwFlags |= WHDR_DONE;            /* the feeder fills it first */
    }
    HANDLE t = CreateThread(NULL, 0, feeder, ev, 0, NULL);
    if (!t) return -1;
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    psp_audio_set_sink(sink);
    return 0;
}

#else

int mhfu_audio_start(void) { (void)mix; psp_audio_set_sink(sink); return -1; }

#endif
