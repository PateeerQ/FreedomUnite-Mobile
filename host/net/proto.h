/* MHFU-Android -- the room protocol between a game and the hub.
 *
 * A hub holds rooms. A room is one virtual ad hoc "air": everyone in it can
 * see each other's groups and exchange PDP/PTP traffic, as if their PSPs were
 * in wireless range. Each player has one TCP connection to the hub and every
 * packet goes through it, so nobody needs a public address or a forwarded
 * port: the connection is outbound, and the hub relays.
 *
 * The hub can run inside the app of whoever pressed "Create Server" (same
 * Wi-Fi or hotspot, found by LAN broadcast), or as a standalone relay on a
 * server (tools/mhfu_hub.c) for play over the internet. The protocol is the
 * same for both.
 *
 * Framing: u32 length (of type + body), u8 type, body. All integers are
 * little-endian. MACs are the PSP's 6-byte ether addresses; the hub stamps
 * every packet's source MAC itself, so nobody can speak for someone else.
 */
#ifndef MHFU_NET_PROTO_H
#define MHFU_NET_PROTO_H

#include <stdint.h>
#include <string.h>

#define MHN_MAGIC        0x324E484Du   /* "MHN2": rooms have a name and maybe a passcode */
#define MHN_PORT         27313         /* hub TCP port; LAN discovery is UDP on the same number */
#define MHN_CODE_LEN     8             /* "MH-4K7Q" plus NUL */
#define MHN_PASS_LEN     8             /* a room's passcode, 4 digits as in Monster Hunter; "" for none */
#define MHN_NAME_LEN     32            /* a room's name in the list: "Fathir's room" */
#define MHN_NICK_LEN     128           /* SceNetAdhocctlNickname */
#define MHN_GROUP_LEN    8             /* SceNetAdhocctlGroupName */
#define MHN_MAX_FRAME    (70 * 1024)   /* PDP's largest datagram is 65523 */
#define MHN_MAX_MEMBERS  16

enum {
    /* client -> hub */
    MHN_HELLO = 1,          /* u32 magic, u8 create, char code[8], u8 mac[6], char nick[128], char pass[8]:
                             * a create sets the room's passcode, a join must match it */
    /* hub -> client */
    MHN_WELCOME = 2,        /* u32 status, char code[8] */
    MHN_MEMBER = 3,         /* u8 joined, u8 mac[6], char nick[128]: someone entered or left the room */
    /* the room list: instead of a HELLO, then the hub answers and the client hangs up */
    MHN_LIST = 4,           /* c->h, no body */
    MHN_ROOM_LIST = 5,      /* h->c: u32 n, n x room entry (below) */

    /* adhocctl: the game's own groups inside the room */
    MHN_CTL_CONNECT = 10,   /* c->h: char group[8]. Create, Connect and Join all mean "be in this group" */
    MHN_CTL_DISCONNECT = 11,/* c->h */
    MHN_CTL_SCAN = 12,      /* c->h */
    MHN_CTL_SCAN_RESULT = 13, /* h->c: u32 n, n x { char group[8], u8 bssid[6] } */
    MHN_CTL_CONNECTED = 14, /* h->c: char group[8], u8 bssid[6] */
    MHN_CTL_PEER = 15,      /* h->c: u8 joined, u8 mac[6], char nick[128]: a peer in my group */

    /* traffic: u8 src[6], u16 sport, u8 dst[6], u16 dport, then the payload.
     * The hub routes on dst (FF:FF:FF:FF:FF:FF is everyone in the sender's
     * group) and overwrites src with the sender's MAC. */
    MHN_PDP = 20,
    MHN_PTP_SYN = 21,       /* connect request */
    MHN_PTP_ACK = 22,       /* accepted */
    MHN_PTP_RST = 23,       /* refused, or the connection is gone */
    MHN_PTP_DATA = 24,
    MHN_PTP_FIN = 25,       /* closed */

    MHN_PING = 30,          /* either way: keeps NATs and idle sockets alive. A body (u64 the sender's
                             * clock, ms) asks the hub for a PONG with the same body: the round trip */

    /* The room's chat (MHFU has none of its own; Select opens it, chat.c).
     * Hubs that predate it ignore it, as they ignore any type they do not know. */
    MHN_CHAT = 31,          /* c->h: UTF-8 text, 1..MHN_CHAT_MAX bytes, no NUL.
                             * h->c: u8 mac[6], char nick[128], then the text: to everyone else in the room */
    MHN_PONG = 32,          /* h->c: a PING's body, back at once (hubs before it send none: no ping shown) */

    /* The room's frame rate: everyone plays at the slowest one's, since the
     * game's own netcode goes by frames. A player who never says counts as 30. */
    MHN_FPS = 33,           /* c->h: u8 the frame rate this player can play at (30 or 60) */
    MHN_ROOM_FPS = 34,      /* h->c: u8 the room's frame rate, char nick[128] of the slowest player (or "") */
};

#define MHN_CHAT_MAX     200           /* bytes of one message */
#define MHN_CHAT_BURST   5             /* a hub passes at most this many messages from one player ... */
#define MHN_CHAT_WINDOW_MS 10000       /* ... in this long; more are dropped */

enum {
    MHN_OK = 0,
    MHN_NO_ROOM = 1,        /* no room with that code */
    MHN_ROOM_FULL = 2,
    MHN_BAD_VERSION = 3,
    MHN_MAC_IN_USE = 4,
    MHN_BAD_PASSCODE = 5,
};

/* A room in a list: char code[8], char name[32], u8 players, u8 locked. The
 * LAN form: a UDP "MHNL" broadcast, answered by every hub with "MHNR", u16
 * its TCP port, u8 n, then n of these. Both lists (that one and MHN_ROOM_LIST)
 * then carry n bytes more: each room's frame rate (30 or 60, its slowest
 * member's). Hubs before it send none, and clients before it read only the n
 * entries. */
#define MHN_ENTRY_LEN (MHN_CODE_LEN + MHN_NAME_LEN + 2)
#define MHN_LIST_MAX  64

#define MHN_ADDR_LEN 16     /* the src/sport/dst/dport header of traffic frames */

static inline void mhn_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void mhn_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t mhn_get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t mhn_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int mhn_is_broadcast(const uint8_t *mac) {
    static const uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    return memcmp(mac, bc, 6) == 0;
}

/* Reassembles frames from a TCP byte stream. */
typedef struct {
    uint8_t *buf;
    uint32_t len, cap;
} mhn_reader;

/* Room codes: "MH-" and four characters with no look-alikes (no 0/O, 1/I/L). */
#define MHN_CODE_ALPHABET "23456789ABCDEFGHJKMNPQRSTUVWXYZ"

#endif
