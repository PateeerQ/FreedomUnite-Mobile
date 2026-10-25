/* MHFU-Android -- RetroAchievements while playing, through rcheevos' rc_client.
 *
 * The launcher signs the player in (MHFU_RA_USER and MHFU_RA_TOKEN, a login
 * token, never the password); on Android the app does (MhfuActivity), and may
 * sign in or out while the game runs (mhfu_ach_login / mhfu_ach_logout). The game is identified by the hash of the
 * player's ISO; every game frame rc_client checks the achievements against
 * guest memory (RetroAchievements' PSP addresses are 0x08000000 + address),
 * and an unlock is sent to the server and shown as a toast in the window
 * (window.h). Softcore only: hardcore is for emulators RetroAchievements
 * has approved. */
#ifndef MHFU_ACHIEVEMENTS_H
#define MHFU_ACHIEVEMENTS_H

/* Signs in and loads the game's set in the background. Nothing happens
 * without MHFU_RA_USER/MHFU_RA_TOKEN. `cache_dir` holds the badge images. */
void mhfu_ach_start(const char *iso_path, const char *cache_dir);

/* Once a game frame, on the game's thread. */
void mhfu_ach_frame(void);

/* Signs in with a token now (after mhfu_ach_start), or out. Any thread. */
void mhfu_ach_login(const char *user, const char *token);
void mhfu_ach_logout(void);

/* Who is signed in (the display name), or "". */
const char *mhfu_ach_user(void);

#endif
