/* MHFU-Android -- the table of recompiled overlays.
 *
 * setup.ps1 writes work/gen_ovl/overlays.c from the overlays the host saved
 * (see hle_mhfu.c); without any, host/overlays_none.c supplies an empty one. */
#ifndef MHFU_OVERLAYS_H
#define MHFU_OVERLAYS_H

#include <stdint.h>

typedef struct {
    uint32_t    hash;     /* FNV-1a of header + code + data, as tools/mhfu_ovl prints it */
    uint32_t    base;     /* load address */
    const char *name;     /* from the header, e.g. "demo_sub.ovl" */
    void      (*reg)(void);
} mhfu_overlay;

extern const mhfu_overlay mhfu_overlays[];
extern const int          mhfu_noverlays;

#endif
