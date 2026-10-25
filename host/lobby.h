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

/* All of these block for a few seconds at most; run them off the UI thread.
 * create and join return 0, or -1 with a message for the player in `err`.
 *
 * Rooms work as Monster Hunter's online lobbies do: every open room is listed
 * (who hosts it, how many are in), and a room may have a 4-digit passcode
 * that joining it asks for. The code (MH-4K7Q) still joins directly. */
int  mhfu_lobby_create(const char *passcode, char *err, size_t cap);   /* "" or NULL: no passcode */
int  mhfu_lobby_join(const char *code, const char *passcode, char *err, size_t cap);
void mhfu_lobby_leave(void);

typedef struct {
    char code[8];           /* "MH-4K7Q" */
    char name[32];          /* "Fathir's room" */
    int  players;
    int  locked;            /* has a passcode */
    int  internet;          /* on the relay, rather than this network */
} mhfu_room;

/* The rooms on this network and on the relay (if one is set); how many were
 * written. Takes about a second: it waits for hubs on the LAN to answer. */
int  mhfu_lobby_list(mhfu_room *out, int max);

/* A passcode as typed: "" is fine, else exactly 4 digits. 0 if acceptable. */
int  mhfu_lobby_check_passcode(const char *passcode);

/* One line for a title bar or a banner: "Room MH-4K7Q (LAN), 2 players". */
void mhfu_lobby_status(char *out, size_t cap);
/* The room's code, or "" when not in one. */
void mhfu_lobby_code(char code[8]);

void mhfu_lobby_mac(uint8_t mac[6]);
const char *mhfu_lobby_nick(void);
/* A new name for the rooms this app makes and joins, kept in net.cfg. */
void mhfu_lobby_set_nick(const char *nick);

#endif
