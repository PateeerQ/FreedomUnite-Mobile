/* MHFU-Android -- the room's chat. MHFU has none of its own, so while this
 * player is in a multiplayer room, Select opens a text box over the game
 * (window.h) and the line goes to everyone else in the room (net/client.h,
 * MHN_CHAT). Lines that come in are shown over the game for a while. Out of a
 * room, Select is the game's as always. */
#ifndef MHFU_CHAT_H
#define MHFU_CHAT_H

#include <stdint.h>

/* On every controller read, on the game's thread: takes the buttons and
 * stick about to go to the game and returns the buttons it should see
 * (without Select in a room, none while the chat box is open). */
uint32_t mhfu_chat_buttons(uint32_t buttons, uint8_t *ax, uint8_t *ay);

#endif
