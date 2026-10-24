/* MHFU-Android -- the room hub. See proto.h for what it does. */
#ifndef MHFU_NET_HUB_H
#define MHFU_NET_HUB_H

#include <stdint.h>

typedef struct mhn_hub mhn_hub;

/* Listen on `port` (0 picks one). `lan` also answers LAN broadcasts asking
 * for a room code, which is how "Join Server" finds a hub on the same Wi-Fi
 * without anyone typing an address. `open_rooms` lets a HELLO with create
 * set make a new room (a public relay does; an in-app hub has exactly one
 * room, made by hub_make_room). */
mhn_hub *mhn_hub_new(uint16_t port, int lan, int open_rooms);
uint16_t mhn_hub_port(const mhn_hub *h);

/* A room made up front, for the in-app hub: its code is written to `code`. */
int  mhn_hub_make_room(mhn_hub *h, char code[8]);

/* One pass: accept, read, route. Waits up to `timeout_ms` for something. */
void mhn_hub_poll(mhn_hub *h, int timeout_ms);
/* Poll until mhn_hub_stop, on a thread of its own. */
int  mhn_hub_start_thread(mhn_hub *h);
void mhn_hub_stop(mhn_hub *h);
/* Run on the calling thread until stopped (the standalone server). */
void mhn_hub_run(mhn_hub *h);

/* Free a hub that is not running. */
void mhn_hub_free(mhn_hub *h);

/* Log lines go here (stdout by default). */
void mhn_hub_set_log(void (*fn)(const char *line));

#endif
