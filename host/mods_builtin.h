/* MHFU-Android -- the built-in code mods, as tools/mods_patch.py writes them
 * into work/gen_ovl/mods_builtin.c from mods/builtin (tools/mods_convert.py).
 *
 * A mod is its code and data (loaded at `base`, in memory the host reserves;
 * the code is also recompiled, as the overlay `overlay`) and the hooks that
 * lead the game into it. A hook is a word in the game's code: the recompiled
 * game checks for it there (mods_patch.py) and follows it when it is written.
 * The host writes the EBOOT hooks while a mod is on and the originals back when
 * it is off; overlay hooks the mod writes itself, and they are taken out with it. */
#ifndef MHFU_MODS_BUILTIN_H
#define MHFU_MODS_BUILTIN_H

#include <stdint.h>

typedef struct {
    uint32_t addr, orig, word;
    uint8_t  eboot;          /* in the EBOOT (the host writes it), else in an overlay (the mod does) */
} mhfu_mod_hook;

typedef struct {
    const char *id, *name, *description;
    int         on_by_default;
    uint32_t    base, nwords;
    const uint32_t *words;
    const mhfu_mod_hook *hooks;
    int         nhooks;
    uint32_t    overlay;     /* the recompiled code's hash in mhfu_overlays */
} mhfu_builtin_mod;

extern const mhfu_builtin_mod mhfu_builtin_mods[];
extern const int              mhfu_nbuiltin_mods;

/* Where the mods live: 0x09E10000-0x09E50000, which the game never uses. */
#define MHFU_MODS_LO 0x09E10000u
#define MHFU_MODS_HI 0x09E50000u

#endif
