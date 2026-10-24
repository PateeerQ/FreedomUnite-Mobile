/* MHFU-Android -- desktop bring-up host for Monster Hunter Freedom Unite (EU,
 * ULES01213), statically recompiled with psprecomp.
 *
 * What this does, in order:
 *   1. set up guest memory and the HLE firmware
 *   2. load the decrypted EBOOT's PT_LOAD segment at its fixed address
 *   3. reserve the module's memory so the allocator cannot hand it out
 *   4. build the boot thread's stack, $k0 block, $gp and arguments
 *   5. run the entry point, then keep yielding so the threads it started
 *      run, until they all stop, a miss, the call budget or the time limit
 *   6. print what happened (calls, misses, missing firmware, GE work)
 *
 * The recompiled code itself comes from `allegrexrecomp emit` and lives in
 * work/gen/, which is never committed.
 *
 *   mhfu_host <decrypted EBOOT> <root dir> [--iso FILE] [--budget N] [--seconds N]
 *             [--window [SCALE]] [--press SCRIPT] [--snap N] [--name NAME]
 *
 * --window shows the game and takes the keyboard (see window.c); closing it
 * ends the run. --press feeds timed button presses, counted in game frames
 * (see input.h), and --snap N saves every Nth frame to <root>/frames/.
 * --name is what the on-screen keyboard answers (the character name).
 *
 * <root dir> holds disc/ (the extracted ISO tree, so disc0:/PSP_GAME/... maps
 * to <root>/disc/PSP_GAME/...), ms/ (memory stick: saves) and flash0/font/
 * (the PSP's system fonts, for sceLibFont). With --iso the
 * disc is read from the image instead, which MHFU needs: it stats DATA.BIN
 * for its start sector and then opens sector ranges of it by number.
 */
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/recomp_rt.h"
#include "psprecomp/render.h"
#include "psprecomp/vfpu.h"

#include "adhoc.h"
#include "atrac.h"
#include "mpeg.h"
#include "audio.h"
#include "gamepad.h"
#include "hle_mhfu.h"
#include "input.h"
#include "libfont.h"
#include "lobby.h"
#include "utility.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* From work/gen/<prefix>_register.c. */
void psp_recomp_register(void);

#define BOOT_STACK_SIZE  0x40000u      /* 256 KB, a typical user_main stack */
#define K0_BLOCK_SIZE    0x100u
#define BOOT_PATH        "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
#define DEFAULT_BUDGET   2000000000ull
#define DEFAULT_SECONDS  60
/* Yields in a row in which no other thread ran before the boot thread decides no
 * guest thread is runnable any more. */
#define IDLE_YIELDS      1000

/* DEFAULT_HEAP_LO in psprecomp's sysmem.c. MHFU's image (code + bss) runs from
 * 0x08804000 to about 0x09D60000, far past it. */
#define SYSMEM_HEAP_LO   0x08C00000u

static const char *g_root = "work/root";

/* ---- ELF ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct {
    uint32_t entry;
    uint32_t lo, hi;     /* lowest and highest address the image occupies */
    uint32_t gp;         /* from .rodata.sceModuleInfo, 0 if not found */
    char     name[29];
} image_info;

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

/* Place every PT_LOAD segment at its virtual address and zero its bss tail.
 * MHFU's EBOOT is a fixed-address executable (ET_EXEC), not a relocatable PRX,
 * so there is nothing to relocate. */
static int load_elf(const uint8_t *d, size_t len, image_info *img) {
    memset(img, 0, sizeof *img);
    if (len < 52 || memcmp(d, "\x7F" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1) {
        fprintf(stderr, "not a little-endian ELF32 -- is the EBOOT decrypted?\n");
        return -1;
    }
    if (rd16(d + 18) != 8) { fprintf(stderr, "not a MIPS ELF\n"); return -1; }
    if (rd16(d + 16) != 2) {
        fprintf(stderr, "ELF type 0x%04X: only fixed-address executables are handled here\n",
                rd16(d + 16));
        return -1;
    }

    img->entry = rd32(d + 24);
    uint32_t phoff = rd32(d + 28), shoff = rd32(d + 32);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    uint16_t shentsize = rd16(d + 46), shnum = rd16(d + 48), shstrndx = rd16(d + 50);
    img->lo = 0xFFFFFFFFu;

    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + (size_t)i * phentsize;
        if ((size_t)(ph - d) + 32 > len || rd32(ph) != 1 /* PT_LOAD */) continue;
        uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8);
        uint32_t filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        if ((size_t)off + filesz > len) { fprintf(stderr, "segment %u runs past the file\n", i); return -1; }

        if (psp_mem_write_block(vaddr, d + off, filesz) != 0) {
            fprintf(stderr, "cannot place segment %u at 0x%08X\n", i, vaddr);
            return -1;
        }
        if (memsz > filesz) {
            void *bss = psp_mem_ptr(vaddr + filesz, memsz - filesz);
            if (!bss) { fprintf(stderr, "bss of segment %u is unmapped\n", i); return -1; }
            memset(bss, 0, memsz - filesz);
        }
        if (vaddr < img->lo) img->lo = vaddr;
        if (vaddr + memsz > img->hi) img->hi = vaddr + memsz;
        printf("segment %u  0x%08X  file %u  mem %u\n", i, vaddr, filesz, memsz);
    }
    if (img->hi == 0) { fprintf(stderr, "no PT_LOAD segment\n"); return -1; }

    /* sceModuleInfo: u16 attribute, u8 version[2], char name[28], u32 gp, ... */
    if (shoff && shstrndx < shnum && (size_t)shoff + (size_t)shnum * shentsize <= len) {
        const uint8_t *strsh = d + shoff + (size_t)shstrndx * shentsize;
        uint32_t stroff = rd32(strsh + 16);
        for (uint16_t i = 0; i < shnum; i++) {
            const uint8_t *sh = d + shoff + (size_t)i * shentsize;
            const char *nm = (const char *)d + stroff + rd32(sh);
            if (strcmp(nm, ".rodata.sceModuleInfo") != 0) continue;
            uint32_t off = rd32(sh + 16);
            if ((size_t)off + 36 > len) break;
            memcpy(img->name, d + off + 4, 28);
            img->gp = rd32(d + off + 32);
            break;
        }
    }
    return 0;
}

/* ---- run control ---------------------------------------------------------- */

static void report(void) {
    printf("\n=== report ===\n");
    printf("dispatch calls     %llu\n", (unsigned long long)psp_dispatch_calls());
    printf("dispatch misses    %llu\n", (unsigned long long)psp_dispatch_misses());
    printf("last function      0x%08X\n", psp_trace_last());
    printf("unimplemented fw   %llu calls\n", (unsigned long long)psp_hle_unimplemented_calls());
    printf("GE commands        %llu  vertices %llu\n",
           (unsigned long long)psp_ge_command_count(), (unsigned long long)psp_ge_vertex_count());
    printf("GE callbacks run   %llu\n", (unsigned long long)psp_ge_callbacks_run());
    psp_ge_dump_stats(stdout);
    /* MHFU_PEEK=addr[,words]: memory as the run left it, e.g. a table found
     * through a pointer that only the end state can resolve. */
    if (getenv("MHFU_PEEK")) {
        unsigned long a = 0, n = 8;
        sscanf(getenv("MHFU_PEEK"), "%lx,%lu", &a, &n);
        printf("peek 0x%08lX:", a);
        for (unsigned long i = 0; i < n; i++) printf(" %08X", psp_read32((uint32_t)(a + i * 4)));
        printf("\n");
    }
    psp_vfpu_report(stdout);
    printf("font glyphs drawn  %llu\n", (unsigned long long)mhfu_libfont_glyphs());
    printf("game frames        %llu  (controller reads)\n", (unsigned long long)mhfu_input_frames());
    printf("VTimer handlers    %llu  virtual time %.2f s\n", (unsigned long long)mhfu_vtimer_fired(),
           psp_threadman_vtime_us() / 1e6);
    printf("thread switches    %llu\n", (unsigned long long)psp_threadman_switches());
    /* Stack imbalances by site: a callee that returns with $sp moved is how a
     * caller later reads its saved $ra from someone else's frame. */
    psp_sp_site sites[24];
    int nsites = psp_sp_call_hist(sites, 24);
    if (nsites) printf("sp not restored by callee (site, delta, count):");
    for (int i = 0; i < nsites; i++)
        printf("%s %08X %+d x%llu", i % 4 == 0 ? "\n   " : "", sites[i].site, sites[i].delta,
               (unsigned long long)sites[i].count);
    if (nsites) printf("\n");
    /* The save manager (pointer at 0x09A44C60): +0x0C state (jump table at
     * 0x089321DC, run by 0x088BA870), +0x34 savedata result, +0x970 savedata
     * dialog active, +0x971 message dialog active, +0x978 memory stick inserted. */
    uint32_t sm = psp_read32(0x09A44C60);
    if (sm) {
        printf("save manager       0x%08X  state %u  result 0x%08X  +0x9AC %u  +0x960:", sm,
               psp_read32(sm + 0x0C), psp_read32(sm + 0x34), psp_read8(sm + 0x9AC));
        for (uint32_t o = 0x960; o < 0x9A0; o += 4) printf(" %08X", psp_read32(sm + o));
        printf("\n");
    }

    printf("\nmost-called firmware:\n");
    psp_hle_dump_counts(stdout, 40);
    printf("\nlast firmware calls:\n");
    psp_hle_dump_recent(stdout);
    psp_threadman_dump(stdout);
    printf("\nrecent function entries:\n");
    psp_trace_dump();

    char path[1024];
    snprintf(path, sizeof path, "%s/frame.ppm", g_root);
    if (psp_display_capture(path) == 0) printf("\nframebuffer saved to %s\n", path);
    else printf("\nno framebuffer set yet\n");
    fflush(stdout);
}

static void on_budget(void) {
    printf("\ncall budget exhausted\n");
    report();
    exit(0);
}

/* A guest thread that never waits keeps the CPU baton forever, so the boot
 * thread's own time check would never run. This one does, on its own host
 * thread, and reports whatever state the guest is in. */
static int g_seconds = DEFAULT_SECONDS;
/* The boot thread stops on its own at g_seconds when it gets the CPU; the
 * watchdog only steps in if it does not. */
#define WATCHDOG_GRACE 5

#ifdef _WIN32
static DWORD WINAPI watchdog(LPVOID arg) {
    (void)arg;
    Sleep((DWORD)(g_seconds + WATCHDOG_GRACE) * 1000);
#else
static void *watchdog(void *arg) {
    (void)arg;
    sleep((unsigned)(g_seconds + WATCHDOG_GRACE));
#endif
    printf("\nwatchdog: %d s wall-clock limit reached\n", g_seconds + WATCHDOG_GRACE);
    report();
    _Exit(0);
}

static void start_watchdog(void) {
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog, NULL) == 0) pthread_detach(t);
#endif
}

static void on_window_closed(void) {
    printf("\nwindow closed\n");
    report();
    _Exit(0);
}

static void on_miss(uint32_t addr) {
    int slot = mhfu_overlay_slot(addr);
    if (slot > 0)
        printf("\ndispatch miss: 0x%08X is in overlay slot %d -- code loaded at run time,"
               " not recompiled yet (saved under %s/../overlays)\n", addr, slot, g_root);
    else
        printf("\ndispatch miss: nothing recompiled at 0x%08X\n", addr);
    /* What is there now, and who jumped: a miss inside a recompiled overlay is
     * either a function discovery did not find (code here) or a bad pointer
     * (zeros, data, bss). */
    printf("  ra 0x%08X, words at the target:", psp_cpu.r[PSP_REG_RA]);
    for (uint32_t i = 0; i < 8; i++) printf(" %08X", psp_read32(addr + 4 * i));
    printf("\n  registers:");
    for (int i = 1; i < 32; i++) printf("%s r%d=%08X", i % 8 == 0 ? "\n            " : "", i, psp_cpu.r[i]);
    /* A return to a bad address: where on the stack does the target sit? */
    uint32_t sp = psp_cpu.r[PSP_REG_SP];
    /* The recompiled call stack: each frame is a call still in progress, by
     * the return address its caller expects. */
    printf("\n  call stack (expected returns):");
    int depth = 0;
    for (const psp_frame *f = psp_cpu.frame; f && depth < 24; f = f->prev, depth++) printf(" %08X", f->ret);
    printf("\n  stack slots holding the target (sp-256..sp+256):");
    for (uint32_t a = sp - 256; a < sp + 256; a += 4)
        if (psp_read32(a) == addr) printf(" 0x%08X(sp%+d)", a, (int)(a - sp));
    printf("\n");
    report();
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <decrypted EBOOT> <root dir> [--iso FILE] [--budget N] [--seconds N]"
                " [--window [SCALE]] [--press SCRIPT] [--snap N] [--name NAME]\n"
                "       multiplayer: [--net-create | --net-join CODE] [--nick NAME] [--mac 02:..] [--relay HOST[:PORT]]\n",
                argv[0]);
        return 2;
    }
    /* The runtime logs to stderr; unbuffered stdout keeps both in order in
     * a captured log. */
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *eboot = argv[1];
    g_root = argv[2];
    const char *iso = NULL, *press = NULL, *name = NULL;
    const char *nick = NULL, *mac = NULL, *relay = NULL, *net_join = NULL;
    unsigned long long budget = DEFAULT_BUDGET;
    int window = 0, snap = 0, net_create = 0;
    for (int i = 3; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--window")) {
            window = 2;
            if (more && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9') window = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--net-create")) net_create = 1;
        else if (!more) break;
        else if (!strcmp(argv[i], "--net-join")) net_join = argv[++i];
        else if (!strcmp(argv[i], "--nick"))     nick = argv[++i];
        else if (!strcmp(argv[i], "--mac"))      mac = argv[++i];
        else if (!strcmp(argv[i], "--relay"))    relay = argv[++i];
        else if (!strcmp(argv[i], "--iso"))     iso = argv[++i];
        else if (!strcmp(argv[i], "--budget"))  budget = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seconds")) g_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--press"))   press = argv[++i];
        else if (!strcmp(argv[i], "--snap"))    snap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name"))    name = argv[++i];
    }

    size_t len = 0;
    uint8_t *elf = read_file(eboot, &len);
    if (!elf) { fprintf(stderr, "cannot read %s\n", eboot); return 1; }

    if (psp_mem_init() != 0) { fprintf(stderr, "out of memory\n"); return 1; }
    psp_cpu_reset();
    psp_hle_init();
    psp_io_set_root(g_root);      /* after psp_hle_init: io_init clears it */
    if (iso && psp_io_set_disc_image(iso) != 0) return 1;
    char ovl_dir[600];
    snprintf(ovl_dir, sizeof ovl_dir, "%s/../overlays", g_root);
    mhfu_hle_register(ovl_dir);
    mhfu_atrac_register();
    mhfu_mpeg_register();
    char font_dir[600];
    snprintf(font_dir, sizeof font_dir, "%s/flash0/font", g_root);
    mhfu_libfont_register(font_dir);
    char ms_dir[600];
    snprintf(ms_dir, sizeof ms_dir, "%s/ms", g_root);
    mhfu_utility_register(ms_dir);
    mhfu_utility_set_osk_text(name);
    /* Multiplayer: the room the game's ad hoc play goes through. The window's
     * Multiplayer menu does the same as these two options. */
    mhfu_lobby_init(g_root, nick, mac, relay);
    mhfu_adhoc_register();
    if (net_create || net_join) {
        char err[256], status[128];
        if ((net_create ? mhfu_lobby_create(err, sizeof err) : mhfu_lobby_join(net_join, err, sizeof err)) != 0)
            printf("net: %s\n", err);
        mhfu_lobby_status(status, sizeof status);
        printf("net: %s\n", status);
    }
    char snap_dir[600];
    snprintf(snap_dir, sizeof snap_dir, "%s/frames", g_root);
    if (mhfu_input_init(press, snap, snap_dir) != 0) return 2;
    psp_recomp_register();
    printf("%d recompiled entries registered\n", (int)psp_dispatch_count());

    image_info img;
    if (load_elf(elf, len, &img) != 0) return 1;
    free(elf);
    printf("module %s  entry 0x%08X  image 0x%08X..0x%08X  gp 0x%08X\n",
           img.name[0] ? img.name : "(unknown)", img.entry, img.lo, img.hi, img.gp);

    /* The allocator's heap starts at SYSMEM_HEAP_LO. Claim everything from
     * there to the end of the image first, so no later allocation lands on
     * MHFU's code or bss. */
    if (img.hi > SYSMEM_HEAP_LO) {
        uint32_t got = psp_sysmem_alloc(img.hi - SYSMEM_HEAP_LO, 0);
        if (got != SYSMEM_HEAP_LO) {
            fprintf(stderr, "could not reserve the module image (got 0x%08X)\n", got);
            return 1;
        }
    }
    printf("free user memory   %u KB\n", psp_sysmem_free() / 1024);

    /* Boot thread: stack from the top of memory, a zeroed $k0 block at its top
     * (the firmware keeps per-thread data there), the boot path as argp just
     * below it, and $sp below that. */
    uint32_t stack = psp_sysmem_alloc(BOOT_STACK_SIZE, 1);
    if (!stack) { fprintf(stderr, "no memory for the boot stack\n"); return 1; }
    uint32_t k0 = stack + BOOT_STACK_SIZE - K0_BLOCK_SIZE;
    memset(psp_mem_ptr(k0, K0_BLOCK_SIZE), 0, K0_BLOCK_SIZE);

    uint32_t argp = (k0 - (uint32_t)sizeof BOOT_PATH) & ~0xFu;
    psp_mem_write_block(argp, BOOT_PATH, (uint32_t)sizeof BOOT_PATH);

    psp_cpu.r[PSP_REG_A0] = (uint32_t)sizeof BOOT_PATH;
    psp_cpu.r[PSP_REG_A1] = argp;
    psp_cpu.r[PSP_REG_K0] = k0;
    psp_cpu.r[PSP_REG_GP] = img.gp;
    psp_cpu.r[PSP_REG_SP] = (argp - 0x40) & ~0xFu;
    psp_cpu.r[PSP_REG_RA] = 0;

    psp_set_miss_handler(on_miss);
    psp_dispatch_set_budget(budget, on_budget);

    /* MHFU_RENDER=gl|software|null: the GE backend. gl needs the window; null
     * draws nothing, which shows how much of a frame the rasteriser costs. */
    const char *renderer = getenv("MHFU_RENDER");
    if (renderer && strcmp(renderer, "gl") != 0 && psp_render_select(renderer) != 0)
        fprintf(stderr, "unknown renderer %s\n", renderer);

    if (window) {
        mhfu_window_on_close(on_window_closed);
        if (mhfu_window_start(window) == 0) {
            mhfu_input_set_pacing(1);
            if (renderer && !strcmp(renderer, "gl")) {
                if (mhfu_window_gl() == 0) printf("renderer: OpenGL, %dx internal resolution\n", window);
                else fprintf(stderr, "no OpenGL; using the software renderer\n");
            }
        }
        else fprintf(stderr, "no window (only on Windows); running without one\n");
        if (mhfu_gamepad_start() != 0) fprintf(stderr, "no controller support in this build\n");
        /* Sound only with a window: scripted runs without one stay quiet. */
        if (mhfu_audio_start() != 0) fprintf(stderr, "no audio output; running silent\n");
    }

    printf("running from 0x%08X (budget %llu calls, %d s)\n\n", img.entry, budget, g_seconds);
    /* MHFU_WATCH=0x09A12340: log every write to that address and who made it
     * (psprecomp's write watch), to find what corrupts a value. */
    /* MHFU_WATCH_VALUE=0x09D63810 as well: only writes that leave that value. */
    if (getenv("MHFU_WATCH")) {
        uint32_t at = (uint32_t)strtoul(getenv("MHFU_WATCH"), NULL, 0);
        if (getenv("MHFU_WATCH_VALUE"))
            psp_mem_watch_write_value(at, (uint32_t)strtoul(getenv("MHFU_WATCH_VALUE"), NULL, 0));
        else psp_mem_watch_write(at);
    }
    /* MHFU_TEST_ATAN=1: MHFU's VFPU angle function (0x088147C8: the angle from
     * one point of an object to another, as a 16-bit turn) on eight known
     * directions, against the host's atan2. Then exit. */
    if (getenv("MHFU_TEST_ATAN")) {
        const uint32_t obj = psp_sysmem_alloc(0x1000, 0);
        static const float dir[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 },
                                         { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
        const psp_cpu_state saved = psp_cpu;
        for (int i = 0; i < 8; i++) {
            memset(psp_mem_ptr(obj, 0x1000), 0, 0x1000);
            psp_write32(obj + 2896, psp_f32_to_bits(dir[i][0]));   /* x */
            psp_write32(obj + 2904, psp_f32_to_bits(dir[i][1]));   /* z */
            psp_cpu.r[PSP_REG_A0] = obj;
            psp_cpu.r[PSP_REG_RA] = 0;
            psp_call_from_host(0x088147C8u);
            const uint32_t got = psp_cpu.r[PSP_REG_V0] & 0xFFFF;
            /* The function takes atan2 of (-dz, dx), radians to 65536 a turn. */
            double want = atan2(-(double)dir[i][1], (double)dir[i][0]);
            if (want < 0) want += 6.283185307179586;
            printf("atan test: dx %+.0f dz %+.0f -> 0x%04X (%.1f deg), atan2(-dz, dx) = %.1f deg\n",
                   dir[i][0], dir[i][1], got, got * 360.0 / 65536.0, want * 57.29577951308232);
            psp_cpu = saved;
        }
        fflush(stdout);
        _Exit(0);
    }

    start_watchdog();
    psp_call_from_host(img.entry);
    printf("\nentry returned, v0 = 0x%08X; running the threads it started\n",
           psp_cpu.r[PSP_REG_V0]);

    /* The entry usually just starts user_main and returns. Guest threads only
     * run while the boot thread gives the CPU away, so keep yielding until
     * none of them takes it (all ended or all blocked) or the game exits. */
    time_t start = time(NULL);
    uint64_t last = psp_threadman_handovers();
    int idle = 0;
    while (!psp_exit_requested() && !mhfu_window_closed() && idle < IDLE_YIELDS) {
        psp_threadman_yield();
        uint64_t now = psp_threadman_handovers();
        idle = (now == last) ? idle + 1 : 0;
        last = now;
        if (difftime(time(NULL), start) >= g_seconds) break;
    }
    if (psp_exit_requested()) printf("\nthe game asked to exit\n");
    else if (mhfu_window_closed()) printf("\nwindow closed\n");
    else if (idle >= IDLE_YIELDS) printf("\nno guest thread is running any more\n");
    else printf("\ntime limit reached\n");

    report();
    fflush(stdout);
    _Exit(0);   /* guest threads may still be live; do not tear them down */
}
