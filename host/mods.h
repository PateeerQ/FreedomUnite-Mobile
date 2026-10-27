/* MHFU-Android -- mods: the built-in ones (code patched in at build time by
 * tools/mods_patch.py, switched here) and the player's cheats.
 *
 * Cheats: CWCheat codes, as PPSSPP reads them, from <root>/cheats/ULES01213.ini
 * (`_C1 name` turns one on, `_C0` off; `_L 0x... 0x...` its lines). The file
 * is read again when it changes, so the launcher's Mods page and the Android
 * cheat list apply while the game runs. Codes that write memory work; codes
 * that write the game's code cannot (it is recompiled) and are skipped, as the
 * whole cheat. Cheats pause while in a room (the other players would not
 * share them), and achievements stop unlocking while any is on. */
#ifndef MHFU_MODS_H
#define MHFU_MODS_H

#include <stdint.h>

/* Widescreen: the camera takes the window's aspect and the picture is
 * stretched over it. MHFU_WIDESCREEN=1 at start, or the settings. */
int  mhfu_mod_widescreen(void);
void mhfu_mod_set_widescreen(int on);
/* Called by the patched camera code: the aspect as float bits. */
unsigned mhfu_mod_aspect(unsigned original);

/* The built-in code mods (mods/builtin, host/mods_builtin.h): their memory
 * reserved and filled, their recompiled code registered, each switched on or
 * off as MHFU_MODS says ("damage=0,gathering=1"; a mod not named is as its
 * default, on). After the EBOOT is in memory, before the game runs. */
void mhfu_mods_boot(void);
int  mhfu_mod_count(void);
const char *mhfu_mod_id(int i);
const char *mhfu_mod_name(int i);
const char *mhfu_mod_description(int i);
int  mhfu_mod_on(int i);
/* On or off at any time (from the window's thread too). */
void mhfu_mod_set(int i, int on);
/* The mods write their hooks into overlay code (game_task, game_sub): the
 * originals back in a copy of an overlay at `base`, so its hash is the game's. */
void mhfu_mods_unhook(uint32_t base, uint8_t *buf, uint32_t n);

/* Reads <root>/cheats/ULES01213.ini. */
void mhfu_cheats_init(const char *root);
/* Once a game frame, with the buttons held (for the joker codes). */
void mhfu_cheats_frame(uint32_t buttons);
/* How many cheats are on (0: none). */
int  mhfu_cheats_active(void);

/* All of user memory (0x08800000-0x0A000000) to <root>/../memdump/dump-<time>.bin,
 * for working out where things are (F9 in the Win32 window). 0 when saved. */
int  mhfu_mods_dump(char *path_out, int cap);

/* A cheat line that writes the EBOOT's code (CWCheat offsets 0x4000-0x10E6B8):
 * those cannot work here. For the launcher's and the app's own lists too. */
#define MHFU_CHEAT_CODE_LO 0x00004000u
#define MHFU_CHEAT_CODE_HI 0x0010E6B8u

#endif
