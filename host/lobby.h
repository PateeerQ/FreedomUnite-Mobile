/* MHFU-Android -- "Create Server" / "Join Server": rooms with a short code
 * instead of addresses and port forwarding. The UI (the desktop window's menu,
 * later the Android overlay) calls these; the game's ad hoc calls then go
 * through whatever room this is in (adhoc.c).
 *
 * Create: if a relay is configured, the room is made there and works over the
 * internet; otherwise a hub starts inside this app, for players on the same
 * Wi-Fi or hotspot. Join: the code is looked for on the LAN first, then on the
 * relay. Players never type an address either way.
 *
 * Settings live in <root>/net.cfg (written on first run):
 *     nick=Hunter
 *     mac=02:4D:48:xx:xx:xx      a random locally administered address
 *     relay=host[:port]          optional: a public mhfu_hub */
#ifndef MHFU_LOBBY_H
#define MHFU_LOBBY_H

#include <stddef.h>
#include <stdint.h>

/* Read (or create) <dir>/net.cfg. Non-NULL overrides win over the file and
 * are not saved: `mac` as "02:AA:BB:CC:DD:EE". */
void mhfu_lobby_init(const char *dir, const char *nick, const char *mac, const char *relay);

/* Both block for a few seconds at most; run them off the UI thread. They
 * return 0, or -1 with a message for the player in `err`. */
int  mhfu_lobby_create(char *err, size_t cap);
int  mhfu_lobby_join(const char *code, char *err, size_t cap);
void mhfu_lobby_leave(void);

/* One line for a title bar or a banner: "Room MH-4K7Q (LAN), 2 players". */
void mhfu_lobby_status(char *out, size_t cap);
/* The room's code, or "" when not in one. */
void mhfu_lobby_code(char code[8]);

void mhfu_lobby_mac(uint8_t mac[6]);
const char *mhfu_lobby_nick(void);

#endif
