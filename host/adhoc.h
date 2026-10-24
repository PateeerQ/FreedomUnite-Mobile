/* MHFU-Android -- the PSP's ad hoc network (sceNet, sceNetAdhoc,
 * sceNetAdhocctl, sceWlanDrv) over the room network in net/. Whatever room
 * lobby.c has joined is the "air" the game sees; with no room, the game finds
 * nobody, as a PSP alone would. */
#ifndef MHFU_ADHOC_H
#define MHFU_ADHOC_H

/* Call after psp_hle_init() and mhfu_lobby_init(). */
void mhfu_adhoc_register(void);

/* From the host's interrupt tick: runs the game's adhocctl handlers for
 * whatever happened since (connected, scan done, ...). */
void mhfu_adhoc_tick(void);

#endif
