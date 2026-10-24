/* MHFU-Android -- sceUtility savedata, message dialogs, keyboard. See utility.c. */
#ifndef MHFU_UTILITY_H
#define MHFU_UTILITY_H

/* Call after psp_hle_init(). Saves go under `ms_dir`/PSP/SAVEDATA. */
void mhfu_utility_register(const char *ms_dir);

/* What the on-screen keyboard "types" (the character name). Default Hunter. */
void mhfu_utility_set_osk_text(const char *text);

#endif
