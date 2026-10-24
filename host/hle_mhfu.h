/* MHFU-Android -- firmware MHFU needs beyond psprecomp's. */
#ifndef MHFU_HLE_MHFU_H
#define MHFU_HLE_MHFU_H

#include <stdint.h>

/* Call after psp_hle_init(). `overlay_dir` is where loaded overlays are
 * saved (see hle_mhfu.c); NULL to not save them. */
void mhfu_hle_register(const char *overlay_dir);

/* Which overlay slot holds `addr`, or -1. For the miss report. */
int mhfu_overlay_slot(uint32_t addr);

/* Overlays loaded this run that have no recompiled version yet. */
int mhfu_overlays_missing(void);

/* VTimer handler calls so far: MHFU's frame clock is one. */
uint64_t mhfu_vtimer_fired(void);

/* Host monotonic microseconds since the first call: the clock VTimers count. */
uint64_t mhfu_clock_us(void);

#endif
