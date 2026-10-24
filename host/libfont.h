/* MHFU-Android -- sceLibFont, read from PSP PGF fonts. See libfont.c. */
#ifndef MHFU_LIBFONT_H
#define MHFU_LIBFONT_H

#include <stdint.h>

/* Call after psp_hle_init(). `font_dir` holds jpn0.pgf, ltn0.pgf ... (the
 * PSP's flash0:/font); it is read when the game first opens the library. */
void mhfu_libfont_register(const char *font_dir);

/* Glyphs drawn so far, for the report. */
uint64_t mhfu_libfont_glyphs(void);

#endif
