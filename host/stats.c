/* MHFU-Android -- see stats.h. */
#include "stats.h"

#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "audio.h"
#include "input.h"
#include "net/client.h"

#include <stdio.h>

void mhfu_stats_tick(uint64_t now, char *fps_text, size_t cap) {
    static uint64_t last_flips, last_ms;
    uint64_t flips, max_us;
    psp_display_frame_stats(&flips, &max_us);
    if (last_ms && now > last_ms) {
        const double dt = (double)(now - last_ms) / 1000.0;
        const double n = (double)(flips - last_flips);
        if (n > 0)
            snprintf(fps_text, cap, "FPS %.1f   %.1f ms (worst %.1f ms)", n / dt, dt * 1000.0 / n, (double)max_us / 1000.0);
        else
            snprintf(fps_text, cap, "FPS 0   (no frame for %.1f s)", dt);
    }
    /* The log gets the same every 10 seconds, for runs nobody watches. */
    /* With the vblank waits and controller reads per second too: a game that
     * runs at the wrong speed shows which clock is off. */
    static uint64_t log_flips, log_vblanks, log_reads, log_ms;
    if (!log_ms) {
        log_ms = now; log_flips = flips; log_vblanks = psp_display_vblanks(); log_reads = mhfu_input_frames();
    } else if (now - log_ms >= 10000) {
        const double s = (double)(now - log_ms) / 1000.0;
        static uint64_t log_ain, log_ashort, log_adrop, log_aout;
        uint64_t ain, ashort, adrop;
        mhfu_audio_stats(&ain, &ashort, &adrop);
        printf("fps: %.1f over the last %.0f s (vblank waits %.1f/s, controller reads %.1f/s)"
               "  audio: %.0f frames/s in, %.0f out, %llu short, %llu dropped, %.0f ms behind\n",
               (double)(flips - log_flips) / s, s, (double)(psp_display_vblanks() - log_vblanks) / s,
               (double)(mhfu_input_frames() - log_reads) / s, (double)(ain - log_ain) / s,
               (double)(mhfu_audio_out_frames() - log_aout) / s,
               (unsigned long long)(ashort - log_ashort), (unsigned long long)(adrop - log_adrop),
               mhfu_audio_latency_ms());
        log_ain = ain; log_ashort = ashort; log_adrop = adrop; log_aout = mhfu_audio_out_frames();
        /* Ad hoc datagrams, once there are any: how long the game left them
         * waiting is the delay other players see. */
        static uint64_t log_pin, log_pread, log_pdrop, log_pwait;
        uint64_t pin, pread, pdrop, pwait, pmax;
        mhn_pdp_stats(&pin, &pread, &pdrop, &pwait, &pmax);
        if (pin != log_pin || pread != log_pread)
            printf("net: PDP %.0f in/s, %.0f read/s, %llu dropped, waited %.0f ms on average (worst %llu ms)\n",
                   (double)(pin - log_pin) / s, (double)(pread - log_pread) / s,
                   (unsigned long long)(pdrop - log_pdrop),
                   pread > log_pread ? (double)(pwait - log_pwait) / (double)(pread - log_pread) : 0.0,
                   (unsigned long long)pmax);
        log_pin = pin; log_pread = pread; log_pdrop = pdrop; log_pwait = pwait;
        fflush(stdout);
        log_ms = now;
        log_flips = flips;
        log_vblanks = psp_display_vblanks();
        log_reads = mhfu_input_frames();
    }
    last_flips = flips;
    last_ms = now;
}

void mhfu_stats_overlay(int show_fps, const char *fps_text, char *out, size_t cap) {
    char ping[32] = "";
    if (mhn_client_online()) {
        const int ms = mhn_client_ping_ms();
        if (ms >= 0) snprintf(ping, sizeof ping, "Ping %d ms", ms);
        else snprintf(ping, sizeof ping, "Ping --");
    }
    char fps[128];
    snprintf(fps, sizeof fps, "%s%s", fps_text, psp_render_gl_interpolation() ? "  smooth 60" : "");
    if (show_fps && ping[0]) snprintf(out, cap, "%s   %s", fps, ping);
    else if (show_fps) snprintf(out, cap, "%s", fps);
    else snprintf(out, cap, "%s", ping);
}
