/* MHFU-Android -- firmware that MHFU needs and psprecomp does not have yet.
 *
 * Registered after psp_hle_init(), so an entry here also overrides psprecomp's
 * own if it ever gains one. Every NID is the first four bytes of SHA-1 of the
 * name, little-endian (checked when this file was written).
 *
 * The disc is always present and readable: the game's files come from the
 * extracted ISO under <root>/disc, so there is no drive to wait for.
 */
#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle_mhfu.h"
#include "overlays.h"

#include "psprecomp/dispatch.h"

/* ---- sceUmdUser ------------------------------------------------------------ */

#define UMD_PRESENT   0x02
#define UMD_READY     0x10
#define UMD_READABLE  0x20

static void hle_UmdOk(void)          { psp_ret(0); }
static void hle_UmdGetDriveStat(void) { psp_ret(UMD_PRESENT | UMD_READY | UMD_READABLE); }
static void hle_UmdCheckMedium(void)  { psp_ret(1); }

/* (SceUmdDiscInfo *info): { u32 size; u32 type } -- type 0x10 is a game disc. */
static void hle_UmdGetDiscInfo(void) {
    uint32_t info = psp_arg(0);
    if (info) {
        psp_write32(info + 0, 8);
        psp_write32(info + 4, 0x10);
    }
    psp_ret(0);
}

/* ---- sceImpose ------------------------------------------------------------- */

#define LANG_ENGLISH  1
#define BUTTON_CROSS  1   /* X confirms, as on European consoles */

static uint32_t g_lang = LANG_ENGLISH, g_button = BUTTON_CROSS;

/* (int language, int buttonPreference) */
static void hle_ImposeSetLanguageMode(void) {
    g_lang = psp_arg(0);
    g_button = psp_arg(1);
    psp_ret(0);
}

/* (int *language, int *buttonPreference) */
static void hle_ImposeGetLanguageMode(void) {
    if (psp_arg(0)) psp_write32(psp_arg(0), g_lang);
    if (psp_arg(1)) psp_write32(psp_arg(1), g_button);
    psp_ret(0);
}

/* ---- sceUtility ------------------------------------------------------------ */

#define SYSPARAM_ADHOC_CHANNEL     2
#define SYSPARAM_WLAN_POWERSAVE    3
#define SYSPARAM_DATE_FORMAT       4
#define SYSPARAM_TIME_FORMAT       5
#define SYSPARAM_TIMEZONE          6
#define SYSPARAM_DAYLIGHTSAVINGS   7
#define SYSPARAM_LANGUAGE          8
#define SYSPARAM_BUTTON_PREFERENCE 9
#define SCE_UTILITY_INVALID_PARAM  0x80110103

/* (int id, int *value) */
static void hle_UtilityGetSystemParamInt(void) {
    uint32_t v;
    switch (psp_arg(0)) {
    case SYSPARAM_ADHOC_CHANNEL:     v = 0; break;   /* automatic */
    case SYSPARAM_WLAN_POWERSAVE:    v = 0; break;
    case SYSPARAM_DATE_FORMAT:       v = 2; break;   /* DD/MM/YYYY */
    case SYSPARAM_TIME_FORMAT:       v = 0; break;   /* 24 hour */
    case SYSPARAM_TIMEZONE:          v = 0; break;   /* minutes from UTC */
    case SYSPARAM_DAYLIGHTSAVINGS:   v = 0; break;
    case SYSPARAM_LANGUAGE:          v = g_lang; break;
    case SYSPARAM_BUTTON_PREFERENCE: v = g_button; break;
    default: psp_ret(SCE_UTILITY_INVALID_PARAM); return;
    }
    if (psp_arg(1)) psp_write32(psp_arg(1), v);
    psp_ret(0);
}

/* ---- UtilsForUser: cache maintenance ---------------------------------------
 *
 * There is no CPU cache between recompiled code and guest memory, so writing
 * back or invalidating a range has nothing to do. */

static void hle_CacheRange(void) { psp_ret(0); }

/* ---- ThreadManForUser: virtual timers ---------------------------------------
 *
 * A VTimer counts microseconds while started, on the host's monotonic clock.
 * It used to count the scheduler's virtual clock, but every thread's park adds
 * its own timeout to that one clock, so with MHFU's ten threads it ran several
 * times faster than real time. The frame handler then woke user_main more often
 * than it could sleep, user_main never parked, and the loader and sound threads
 * starved: "Now Loading" never finished.
 *
 * A handler fires once the timer's count reaches its schedule:
 *
 *   u32 handler(SceUID uid, SceKernelSysClock *schedule,
 *               SceKernelSysClock *now, void *common)
 *
 * 0 cancels it; anything else is added to the schedule for the next firing.
 * MHFU's frame clock is one of these: the handler wakes user_main and asks to
 * be called again in 16666 us. They run from psprecomp's interrupt tick
 * (psp_intr_set_host_tick, patch 0008), like an interrupt would. */

#define MAX_VTIMERS 16
#define VTIMER_UID_BASE 0x00070000u
#define SCE_KERNEL_ERROR_UNKNOWN_VTID 0x800201BA
/* Firings per timer per tick. Virtual time can jump (a long DelayThread), and
 * a 60 Hz handler should not then run a thousand times in a row. */
#define VTIMER_CATCHUP 4

typedef struct {
    int      used, running;
    uint64_t base_us;      /* virtual time when last started */
    uint64_t accum_us;     /* time counted before that */
    uint32_t handler, common;
    uint64_t schedule;     /* timer count at which the handler fires */
} vtimer;

static vtimer   g_vt[MAX_VTIMERS];
static uint64_t g_vt_fired;

uint64_t mhfu_vtimer_fired(void) { return g_vt_fired; }

static vtimer *find_vt(uint32_t uid) {
    uint32_t i = uid - VTIMER_UID_BASE;
    return (i < MAX_VTIMERS && g_vt[i].used) ? &g_vt[i] : NULL;
}

uint64_t mhfu_clock_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    static uint64_t start;
    uint64_t t = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    if (!start) start = t;
    return t - start;
}

static uint64_t vt_now(const vtimer *v) {
    return v->accum_us + (v->running ? mhfu_clock_us() - v->base_us : 0);
}

static void ret64(uint64_t x) {
    psp_cpu.r[PSP_REG_V0] = (uint32_t)x;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)(x >> 32);
}

static void write_clock(uint32_t at, uint64_t t) {
    psp_write32(at, (uint32_t)t);
    psp_write32(at + 4, (uint32_t)(t >> 32));
}

static void vtimer_tick(void) {
    for (uint32_t i = 0; i < MAX_VTIMERS; i++) {
        vtimer *v = &g_vt[i];
        for (int k = 0; k < VTIMER_CATCHUP; k++) {
            if (!v->used || !v->running || !v->handler) break;
            uint64_t now = vt_now(v);
            if (now < v->schedule) break;
            if (!psp_lookup(v->handler)) {
                fprintf(stderr, "mhfu: VTimer 0x%08X handler 0x%08X is not a recompiled"
                        " function; cancelled\n", VTIMER_UID_BASE + i, v->handler);
                v->handler = 0;
                break;
            }
            uint32_t handler = v->handler;
            psp_cpu_state saved = psp_cpu;
            /* The two SysClocks go on the interrupted thread's stack, below
             * anything it uses. */
            uint32_t sp = (psp_cpu.r[PSP_REG_SP] - 64) & ~15u;
            write_clock(sp + 16, v->schedule);
            write_clock(sp + 24, now);
            psp_cpu.r[PSP_REG_SP] = sp;
            psp_cpu.r[PSP_REG_A0] = VTIMER_UID_BASE + i;
            psp_cpu.r[PSP_REG_A1] = sp + 16;
            psp_cpu.r[PSP_REG_A2] = sp + 24;
            psp_cpu.r[PSP_REG_A3] = v->common;
            psp_cpu.r[PSP_REG_RA] = 0;
            psp_call_from_host(handler);
            uint32_t next = psp_cpu.r[PSP_REG_V0];
            psp_cpu = saved;
            g_vt_fired++;
            /* The handler may have set a new one itself; that wins. */
            if (v->handler != handler) break;
            if (next == 0) { v->handler = 0; break; }
            v->schedule += next;
        }
        /* Still behind after catching up: start counting again from now. */
        if (v->used && v->running && v->handler && vt_now(v) >= v->schedule)
            v->schedule = vt_now(v);
    }
}

/* (const char *name, SceKernelVTimerOptParam *opt) */
static void hle_CreateVTimer(void) {
    for (uint32_t i = 0; i < MAX_VTIMERS; i++) {
        if (g_vt[i].used) continue;
        memset(&g_vt[i], 0, sizeof g_vt[i]);
        g_vt[i].used = 1;
        psp_ret(VTIMER_UID_BASE + i);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
}

static void hle_DeleteVTimer(void) {
    vtimer *v = find_vt(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->used = 0;
    psp_ret(0);
}

/* Both return 1 if the timer was already in the requested state. */
static void hle_StartVTimer(void) {
    vtimer *v = find_vt(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    if (v->running) { psp_ret(1); return; }
    v->running = 1;
    v->base_us = mhfu_clock_us();
    psp_ret(0);
}

static void hle_StopVTimer(void) {
    vtimer *v = find_vt(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    if (!v->running) { psp_ret(1); return; }
    v->accum_us = vt_now(v);
    v->running = 0;
    psp_ret(0);
}

/* (uid, SceKernelSysClock *time) */
static void hle_GetVTimerTime(void) {
    vtimer *v = find_vt(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    if (psp_arg(1)) write_clock(psp_arg(1), vt_now(v));
    psp_ret(0);
}

static void hle_GetVTimerTimeWide(void) {
    vtimer *v = find_vt(psp_arg(0));
    ret64(v ? vt_now(v) : 0);
}

static void set_handler(uint32_t uid, uint64_t schedule, uint32_t handler, uint32_t common) {
    vtimer *v = find_vt(uid);
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->handler = handler;
    v->common = common;
    v->schedule = schedule;
    psp_ret(0);
}

/* (uid, SceKernelSysClock *schedule, handler, common) */
static void hle_SetVTimerHandler(void) {
    uint32_t at = psp_arg(1);
    uint64_t t = at ? psp_read32(at) | (uint64_t)psp_read32(at + 4) << 32 : 0;
    set_handler(psp_arg(0), t, psp_arg(2), psp_arg(3));
}

/* (uid, u64 schedule, handler, common): the u64 takes the aligned pair a2:a3,
 * so the handler is in t0. */
static void hle_SetVTimerHandlerWide(void) {
    set_handler(psp_arg(0), psp_arg(2) | (uint64_t)psp_arg(3) << 32, psp_arg(4), psp_arg(5));
}

static void hle_CancelVTimerHandler(void) {
    vtimer *v = find_vt(psp_arg(0));
    if (!v) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_VTID); return; }
    v->handler = 0;
    psp_ret(0);
}

/* ---- modules, clock, console id ---------------------------------------------
 *
 * Firmware modules (sceUtilityLoadModule: Atrac3plus, Mpeg, the network
 * stack) are HLE here, so loading one only has to succeed. MHFU also loads a
 * PRX of its own out of DATA.BIN with sceKernelLoadModuleByID; its code is
 * not run yet -- the log says when, and the game carries on with a module id
 * as if it had loaded. */

static void hle_UtilityLoadModule(void) {
    fprintf(stderr, "mhfu: sceUtilityLoadModule 0x%04X (HLE, nothing to load)\n", psp_arg(0));
    psp_ret(0);
}
static void hle_UtilityUnloadModule(void) { psp_ret(0); }

static uint32_t g_next_module = 0x00100001u;
static void hle_LoadModuleByID(void) {
    fprintf(stderr, "mhfu: sceKernelLoadModuleByID(fd %d): PRX code is not run yet; returning module 0x%08X\n",
            (int)psp_arg(0), g_next_module);
    psp_ret(g_next_module++);
}

/* (ScePspDateTime *): u16 year, month, day, hour, minute, second; u32 us. */
static void hle_RtcGetCurrentClockLocalTime(void) {
    uint32_t at = psp_arg(0);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (at && t) {
        psp_write16(at,      (uint16_t)(t->tm_year + 1900));
        psp_write16(at + 2,  (uint16_t)(t->tm_mon + 1));
        psp_write16(at + 4,  (uint16_t)t->tm_mday);
        psp_write16(at + 6,  (uint16_t)t->tm_hour);
        psp_write16(at + 8,  (uint16_t)t->tm_min);
        psp_write16(at + 10, (uint16_t)t->tm_sec);
        psp_write32(at + 12, 0);
    }
    psp_ret(0);
}

/* (u8 psid[16]): a fixed id. Games use it to tell consoles apart, e.g. in
 * ad hoc play; a real one is per device, which the Android port will need. */
static void hle_OpenPSIDGetOpenPSID(void) {
    static const uint8_t k_id[16] = { 'M','H','F','U','-','R','E','C','O','M','P',0,0,0,0,1 };
    uint32_t at = psp_arg(0);
    if (at) for (uint32_t i = 0; i < 16; i++) psp_write8(at + i, k_id[i]);
    psp_ret(0);
}

/* (u8 mac[6]): the console's wireless MAC. MHFU reads it for ad hoc play;
 * a locally administered fixed address until the Android port has one per
 * device. */
static void hle_WlanGetEtherAddr(void) {
    static const uint8_t k_mac[6] = { 0x02, 'M', 'H', 'F', 'U', 0x01 };
    uint32_t at = psp_arg(0);
    if (at) for (uint32_t i = 0; i < 6; i++) psp_write8(at + i, k_mac[i]);
    psp_ret(0);
}

/* ---- sceGe_user / SysMemUserForUser ---------------------------------------- */

/* (u32 translation) -> the previous value. Only matters to a GPU that maps
 * EDRAM addresses differently; the PSP's default is 0x400. */
static uint32_t g_edram_translation = 0x400;
static void hle_GeEdramSetAddrTranslation(void) {
    uint32_t old = g_edram_translation;
    g_edram_translation = psp_arg(0);
    psp_ret(old);
}

/* Records the SDK the game was built with; nothing here depends on it. */
static void hle_SetCompiledSdkVersion(void) { psp_ret(0); }

/* ---- overlays ----------------------------------------------------------------
 *
 * MHFU keeps part of its code in DATA.BIN and loads it at run time into fixed
 * slots inside the EBOOT's bss. The slot table is six start addresses at
 * 0x089CBA70 (slot 0 is the EBOOT itself; slot 5's start is the end of slot
 * 4). The loader (0x0890DD9C) copies an overlay in, calls
 * sceKernelIcacheInvalidateAll, clears the overlay's bss and runs its
 * constructor table.
 *
 * Each overlay starts with an "MWo3" header (see tools/mhfu_ovl.c). At
 * IcacheInvalidateAll every slot is checked: when its overlay changed, the
 * old overlay's functions are unregistered and the new one's recompiled
 * functions registered, found by a hash of header + code + data. An overlay
 * with no recompiled version yet is saved to <overlay_dir>/slot<N>_<hash>.bin
 * -- setup.ps1 recompiles every file there on its next run. */

#define OVL_TABLE 0x089CBA70u
#define OVL_SLOTS 6
#define OVL_MAGIC 0x336F574Du
#define OVL_HDR   0x40u

static char     g_ovl_dir[512];
static uint32_t g_ovl_hash[OVL_SLOTS];
static int      g_ovl_missing;

static uint32_t ovl_start(int i) { return psp_read32(OVL_TABLE + 4u * (uint32_t)i); }

int mhfu_overlay_slot(uint32_t addr) {
    for (int i = 1; i + 1 < OVL_SLOTS; i++) {
        uint32_t a = ovl_start(i), b = ovl_start(i + 1);
        if (a && b > a && addr >= a && addr < b) return i;
    }
    return -1;
}

int mhfu_overlays_missing(void) { return g_ovl_missing; }

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static const mhfu_overlay *find_overlay(uint32_t hash) {
    for (int i = 0; i < mhfu_noverlays; i++)
        if (mhfu_overlays[i].hash == hash) return &mhfu_overlays[i];
    return NULL;
}

static void save_overlay(int slot, uint32_t hash, const uint8_t *buf, uint32_t n) {
    if (!g_ovl_dir[0]) return;
    char path[700];
    snprintf(path, sizeof path, "%s/slot%d_%08X.bin", g_ovl_dir, slot, hash);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }                  /* saved by an earlier run */
    if ((f = fopen(path, "wb")) == NULL) {
        fprintf(stderr, "mhfu: cannot write %s\n", path);
        return;
    }
    fwrite(buf, 1, n, f);
    fclose(f);
    fprintf(stderr, "mhfu: saved %s\n", path);
}

static void check_overlays(void) {
    for (int i = 1; i + 1 < OVL_SLOTS; i++) {
        uint32_t a = ovl_start(i), b = ovl_start(i + 1);
        if (!a || b <= a) continue;

        uint32_t hash = 0, n = 0;
        if (psp_read32(a) == OVL_MAGIC) {
            n = OVL_HDR + psp_read32(a + 12) + psp_read32(a + 16);
            if (n > b - a) n = 0;
        }
        uint8_t *buf = NULL;
        if (n) {
            buf = (uint8_t *)malloc(n);
            if (!buf) return;
            for (uint32_t k = 0; k < n; k++) buf[k] = psp_read8(a + k);
            hash = fnv1a(buf, n);
        }
        if (hash == g_ovl_hash[i]) { free(buf); continue; }

        psp_unregister_range(a, b);                /* the old overlay is gone */
        g_ovl_hash[i] = hash;
        if (!hash) {
            fprintf(stderr, "mhfu: overlay slot %d emptied\n", i);
            continue;
        }
        char name[33] = {0};
        for (int k = 0; k < 32; k++) name[k] = (char)psp_read8(a + 32 + (uint32_t)k);
        const mhfu_overlay *ov = find_overlay(hash);
        if (ov) {
            ov->reg();
            fprintf(stderr, "mhfu: overlay slot %d: %s (%08X) loaded, recompiled code switched in\n",
                    i, name, hash);
        } else {
            g_ovl_missing++;
            fprintf(stderr, "mhfu: overlay slot %d: %s (%08X) loaded, NOT recompiled yet\n",
                    i, name, hash);
            save_overlay(i, hash, buf, n);
        }
        free(buf);
    }
}

static void hle_IcacheInvalidateAll(void) {
    check_overlays();
    psp_ret(0);
}

/* The scheduler is cooperative, so there is no dispatch to suspend. Suspend
 * returns the previous state (1: was enabled), which Resume is handed back. */
static void hle_SuspendDispatchThread(void) { psp_ret(1); }
static void hle_ResumeDispatchThread(void)  { psp_ret(0); }

void mhfu_hle_register(const char *overlay_dir) {
    snprintf(g_ovl_dir, sizeof g_ovl_dir, "%s", overlay_dir ? overlay_dir : "");
    memset(g_ovl_hash, 0, sizeof g_ovl_hash);

    psp_hle_register(0x920F104A, "UtilsForUser", "sceKernelIcacheInvalidateAll", hle_IcacheInvalidateAll);
    psp_hle_register(0x3AD58B8C, "ThreadManForUser", "sceKernelSuspendDispatchThread",
                     hle_SuspendDispatchThread);
    psp_hle_register(0x27E22EC2, "ThreadManForUser", "sceKernelResumeDispatchThread",
                     hle_ResumeDispatchThread);

    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",               hle_UmdOk);
    psp_hle_register(0xE83742BA, "sceUmdUser", "sceUmdDeactivate",             hle_UmdOk);
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat",           hle_UmdGetDriveStat);
    psp_hle_register(0x20628E6F, "sceUmdUser", "sceUmdGetErrorStat",           hle_UmdOk);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",          hle_UmdOk);
    psp_hle_register(0x4A9E5E29, "sceUmdUser", "sceUmdWaitDriveStatCB",        hle_UmdOk);
    psp_hle_register(0x56202973, "sceUmdUser", "sceUmdWaitDriveStatWithTimer", hle_UmdOk);
    psp_hle_register(0xAEE7404D, "sceUmdUser", "sceUmdRegisterUMDCallBack",    hle_UmdOk);
    psp_hle_register(0xBD2BDE07, "sceUmdUser", "sceUmdUnRegisterUMDCallBack",  hle_UmdOk);
    psp_hle_register(0x46EBB729, "sceUmdUser", "sceUmdCheckMedium",            hle_UmdCheckMedium);
    psp_hle_register(0x340B7686, "sceUmdUser", "sceUmdGetDiscInfo",            hle_UmdGetDiscInfo);

    psp_hle_register(0x36AA6E91, "sceImpose", "sceImposeSetLanguageMode", hle_ImposeSetLanguageMode);
    psp_hle_register(0x24FD7BCF, "sceImpose", "sceImposeGetLanguageMode", hle_ImposeGetLanguageMode);

    psp_hle_register(0xA5DA2406, "sceUtility", "sceUtilityGetSystemParamInt",
                     hle_UtilityGetSystemParamInt);

    psp_hle_register(0xBFA98062, "UtilsForUser", "sceKernelDcacheInvalidateRange",          hle_CacheRange);
    psp_hle_register(0x3EE30821, "UtilsForUser", "sceKernelDcacheWritebackRange",           hle_CacheRange);
    psp_hle_register(0x34B9FA9E, "UtilsForUser", "sceKernelDcacheWritebackInvalidateRange", hle_CacheRange);

    psp_hle_register(0xB77905EA, "sceGe_user", "sceGeEdramSetAddrTranslation",
                     hle_GeEdramSetAddrTranslation);
    psp_hle_register(0x91DE343C, "SysMemUserForUser", "sceKernelSetCompiledSdkVersion500_505",
                     hle_SetCompiledSdkVersion);

    psp_hle_register(0x20FFF560, "ThreadManForUser", "sceKernelCreateVTimer",        hle_CreateVTimer);
    psp_hle_register(0x328F9E52, "ThreadManForUser", "sceKernelDeleteVTimer",        hle_DeleteVTimer);
    psp_hle_register(0xC68D9437, "ThreadManForUser", "sceKernelStartVTimer",         hle_StartVTimer);
    psp_hle_register(0xD0AEEE87, "ThreadManForUser", "sceKernelStopVTimer",          hle_StopVTimer);
    psp_hle_register(0x034A921F, "ThreadManForUser", "sceKernelGetVTimerTime",       hle_GetVTimerTime);
    psp_hle_register(0xC0B3FFD2, "ThreadManForUser", "sceKernelGetVTimerTimeWide",   hle_GetVTimerTimeWide);
    psp_hle_register(0xD8B299AE, "ThreadManForUser", "sceKernelSetVTimerHandler",    hle_SetVTimerHandler);
    psp_hle_register(0x53B00E9A, "ThreadManForUser", "sceKernelSetVTimerHandlerWide", hle_SetVTimerHandlerWide);
    psp_hle_register(0xD2D615EF, "ThreadManForUser", "sceKernelCancelVTimerHandler", hle_CancelVTimerHandler);
    memset(g_vt, 0, sizeof g_vt);

    psp_hle_register(0x2A2B3DE0, "sceUtility", "sceUtilityLoadModule",   hle_UtilityLoadModule);
    psp_hle_register(0xE49BFE92, "sceUtility", "sceUtilityUnloadModule", hle_UtilityUnloadModule);
    psp_hle_register(0xB7F46618, "ModuleMgrForUser", "sceKernelLoadModuleByID", hle_LoadModuleByID);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime", hle_RtcGetCurrentClockLocalTime);
    psp_hle_register(0xC69BEBCE, "sceOpenPSID", "sceOpenPSIDGetOpenPSID", hle_OpenPSIDGetOpenPSID);
    psp_hle_register(0x0C622081, "sceWlanDrv", "sceWlanGetEtherAddr", hle_WlanGetEtherAddr);
    psp_intr_set_host_tick(vtimer_tick);
}
