/* MHFU-Android -- the room hub. See hub.h and proto.h.
 *
 * Single-threaded: one poll over the listener, the LAN discovery socket and
 * every client, then route what came in. A room holds up to 16 players; the
 * game's own ad hoc groups live inside it (a Gathering Hall room is one).
 */
#include "hub.h"
#include "proto.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS   (NET_POLL_MAX - 2)
#define MAX_ROOMS     256
#define IDLE_DROP_MS  60000      /* a client silent this long is gone (clients ping every 10 s) */
#define ROOM_KEEP_MS  600000     /* an empty public room keeps its code this long */

typedef struct {
    net_sock s;
    uint8_t *buf;
    uint32_t len, cap;
    int room;                    /* -1 until HELLO */
    uint8_t mac[6];
    char nick[MHN_NICK_LEN];
    char group[MHN_GROUP_LEN];
    int in_group;
    uint64_t group_seq;          /* order of joining: the earliest member hosts the group */
    uint64_t last_ms;
    uint64_t since_ms;           /* when it connected */
    uint64_t win_ms, win_bytes;  /* what it sent in the current second */
    uint64_t chat_ms[MHN_CHAT_BURST];   /* when its last few chat lines came */
    int fps;                     /* what it can play at (MHN_FPS); 30 until it says */
    int dead;
    char ip[48];
} client;

typedef struct {
    int used;
    char code[MHN_CODE_LEN];
    char pass[MHN_PASS_LEN];     /* "" when anyone may join */
    char name[MHN_NAME_LEN];     /* for the list: whose room it is */
    char by_ip[48];              /* who made it (rooms_per_ip) */
    uint64_t empty_since;        /* 0 while anyone is in it */
} room;

struct mhn_hub {
    net_sock lsock, usock;
    uint16_t port;
    int open_rooms;
    client *cl[MAX_CLIENTS];
    int ncl;
    room rooms[MAX_ROOMS];
    uint64_t seq;
    volatile int stop, running;
    mhn_hub_limits lim;
    uint64_t win_ms, win_bytes;  /* what everyone sent in the current second */
    uint64_t next_note_ms;       /* limit messages, at most one a second */
};

void mhn_hub_set_limits(mhn_hub *h, const mhn_hub_limits *lim) { h->lim = *lim; }

static void default_log(const char *line) { printf("%s\n", line); fflush(stdout); }
static void (*g_log)(const char *) = default_log;
void mhn_hub_set_log(void (*fn)(const char *line)) { g_log = fn ? fn : default_log; }

static void logf_(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_log(line);
}

static void mac_str(const uint8_t *m, char *out) {
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---- rooms ---- */

static int find_room(mhn_hub *h, const char *code) {
    for (int i = 0; i < MAX_ROOMS; i++)
        if (h->rooms[i].used && strncmp(h->rooms[i].code, code, MHN_CODE_LEN) == 0) return i;
    return -1;
}

static int rooms_open(mhn_hub *h, const char *by_ip) {
    int n = 0;
    for (int i = 0; i < MAX_ROOMS; i++)
        if (h->rooms[i].used && (!by_ip || !strcmp(h->rooms[i].by_ip, by_ip))) n++;
    return n;
}

static int new_room(mhn_hub *h) {
    if (h->lim.max_rooms && rooms_open(h, NULL) >= h->lim.max_rooms) return -1;
    int slot = -1;
    for (int i = 0; i < MAX_ROOMS && slot < 0; i++) if (!h->rooms[i].used) slot = i;
    if (slot < 0) return -1;
    char code[MHN_CODE_LEN];
    for (int tries = 0; tries < 100; tries++) {
        uint8_t r[4];
        net_random(r, sizeof r);
        const size_t na = sizeof MHN_CODE_ALPHABET - 1;
        snprintf(code, sizeof code, "MH-%c%c%c%c", MHN_CODE_ALPHABET[r[0] % na], MHN_CODE_ALPHABET[r[1] % na],
                 MHN_CODE_ALPHABET[r[2] % na], MHN_CODE_ALPHABET[r[3] % na]);
        if (find_room(h, code) < 0) {
            room *rm = &h->rooms[slot];
            memset(rm, 0, sizeof *rm);
            rm->used = 1;
            memcpy(rm->code, code, sizeof code);
            rm->empty_since = net_now_ms();
            return slot;
        }
    }
    return -1;
}

static void room_label(room *rm, const char *pass, const char *nick) {
    snprintf(rm->pass, sizeof rm->pass, "%s", pass ? pass : "");
    snprintf(rm->name, sizeof rm->name, "%.24s's room", nick && nick[0] ? nick : "Hunter");
}

int mhn_hub_make_room(mhn_hub *h, char code[8], const char *pass, const char *owner) {
    int r = new_room(h);
    if (r < 0) return -1;
    room_label(&h->rooms[r], pass, owner);
    memcpy(code, h->rooms[r].code, MHN_CODE_LEN);
    logf_("hub: room %s ready%s", code, h->rooms[r].pass[0] ? " (passcode)" : "");
    return 0;
}

static int room_members(mhn_hub *h, int r) {
    int n = 0;
    for (int i = 0; i < h->ncl; i++) if (!h->cl[i]->dead && h->cl[i]->room == r) n++;
    return n;
}

static int room_fps_of(mhn_hub *h, int r, const client **slow);

/* The rooms anyone is in, as list entries, and each one's frame rate into
 * `fps` (the list's trailer); returns how many were written. */
static int list_entries(mhn_hub *h, uint8_t *out, uint8_t *fps, int max) {
    int n = 0;
    for (int r = 0; r < MAX_ROOMS && n < max; r++) {
        if (!h->rooms[r].used) continue;
        const int members = room_members(h, r);
        if (!members) continue;
        uint8_t *e = out + n * MHN_ENTRY_LEN;
        memcpy(e, h->rooms[r].code, MHN_CODE_LEN);
        memcpy(e + MHN_CODE_LEN, h->rooms[r].name, MHN_NAME_LEN);
        e[MHN_CODE_LEN + MHN_NAME_LEN] = (uint8_t)members;
        e[MHN_CODE_LEN + MHN_NAME_LEN + 1] = h->rooms[r].pass[0] ? 1 : 0;
        fps[n] = (uint8_t)room_fps_of(h, r, NULL);
        n++;
    }
    return n;
}

/* ---- sending ---- */

static void send_frame(client *c, int type, const void *body, uint32_t len) {
    if (c->dead) return;
    uint8_t stackbuf[2048];
    uint8_t *f = len + 5 <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(len + 5);
    if (!f) { c->dead = 1; return; }
    mhn_put32(f, len + 1);
    f[4] = (uint8_t)type;
    if (len) memcpy(f + 5, body, len);
    if (net_send_all(c->s, f, len + 5) != 0) c->dead = 1;
    if (f != stackbuf) free(f);
}

static void send_member(client *to, int type, int joined, const client *who) {
    uint8_t b[1 + 6 + MHN_NICK_LEN];
    b[0] = (uint8_t)joined;
    memcpy(b + 1, who->mac, 6);
    memcpy(b + 7, who->nick, MHN_NICK_LEN);
    send_frame(to, type, b, sizeof b);
}

static int same_group(const client *a, const client *b) {
    return a->in_group && b->in_group && a->room == b->room &&
           memcmp(a->group, b->group, MHN_GROUP_LEN) == 0;
}

/* The group's host (its BSSID): whoever has been in it longest. */
static const client *group_host(mhn_hub *h, int room_i, const char *group) {
    const client *best = NULL;
    for (int i = 0; i < h->ncl; i++) {
        const client *c = h->cl[i];
        if (c->dead || c->room != room_i || !c->in_group || memcmp(c->group, group, MHN_GROUP_LEN)) continue;
        if (!best || c->group_seq < best->group_seq) best = c;
    }
    return best;
}

static void leave_group(mhn_hub *h, client *c) {
    if (!c->in_group) return;
    for (int i = 0; i < h->ncl; i++) {
        client *o = h->cl[i];
        if (o != c && same_group(o, c)) send_member(o, MHN_CTL_PEER, 0, c);
    }
    c->in_group = 0;
}

static void join_group(mhn_hub *h, client *c, const char *group) {
    leave_group(h, c);
    memcpy(c->group, group, MHN_GROUP_LEN);
    c->in_group = 1;
    c->group_seq = ++h->seq;
    const client *host = group_host(h, c->room, c->group);
    uint8_t b[MHN_GROUP_LEN + 6];
    memcpy(b, c->group, MHN_GROUP_LEN);
    memcpy(b + MHN_GROUP_LEN, host->mac, 6);
    send_frame(c, MHN_CTL_CONNECTED, b, sizeof b);
    for (int i = 0; i < h->ncl; i++) {
        client *o = h->cl[i];
        if (o == c || o->dead || !same_group(o, c)) continue;
        send_member(c, MHN_CTL_PEER, 1, o);
        send_member(o, MHN_CTL_PEER, 1, c);
    }
}

static void send_scan(mhn_hub *h, client *c) {
    uint8_t b[4 + MHN_MAX_MEMBERS * (MHN_GROUP_LEN + 6)];
    uint32_t n = 0;
    for (int i = 0; i < h->ncl && n < MHN_MAX_MEMBERS; i++) {
        const client *o = h->cl[i];
        if (o->dead || o->room != c->room || !o->in_group) continue;
        int dup = 0;
        for (uint32_t k = 0; k < n && !dup; k++)
            dup = memcmp(b + 4 + k * (MHN_GROUP_LEN + 6), o->group, MHN_GROUP_LEN) == 0;
        if (dup) continue;
        const client *host = group_host(h, o->room, o->group);
        memcpy(b + 4 + n * (MHN_GROUP_LEN + 6), o->group, MHN_GROUP_LEN);
        memcpy(b + 4 + n * (MHN_GROUP_LEN + 6) + MHN_GROUP_LEN, host->mac, 6);
        n++;
    }
    mhn_put32(b, n);
    send_frame(c, MHN_CTL_SCAN_RESULT, b, 4 + n * (MHN_GROUP_LEN + 6));
}

/* ---- traffic ---- */

static void route(mhn_hub *h, client *c, int type, uint8_t *body, uint32_t len) {
    if (len < MHN_ADDR_LEN || c->room < 0) return;
    memcpy(body, c->mac, 6);                  /* the source is whoever sent it */
    const uint8_t *dst = body + 8;
    if (mhn_is_broadcast(dst)) {
        if (type != MHN_PDP) return;          /* only datagrams broadcast */
        for (int i = 0; i < h->ncl; i++) {
            client *o = h->cl[i];
            if (o != c && !o->dead && same_group(o, c)) send_frame(o, type, body, len);
        }
        return;
    }
    for (int i = 0; i < h->ncl; i++) {
        client *o = h->cl[i];
        if (o != c && !o->dead && o->room == c->room && memcmp(o->mac, dst, 6) == 0) {
            send_frame(o, type, body, len);
            return;
        }
    }
    /* Nobody has that address: a connect to it is refused rather than left
     * to time out. */
    if (type == MHN_PTP_SYN || type == MHN_PTP_DATA) {
        uint8_t r[MHN_ADDR_LEN];
        memcpy(r, dst, 6);
        memcpy(r + 6, body + 14, 2);          /* from the absent peer's port */
        memcpy(r + 8, c->mac, 6);
        memcpy(r + 14, body + 6, 2);          /* to the sender's port */
        send_frame(c, MHN_PTP_RST, r, sizeof r);
    }
}

/* ---- the room's chat ---- */

static void chat(mhn_hub *h, client *c, const uint8_t *b, uint32_t len) {
    if (!len || len > MHN_CHAT_MAX) return;
    /* At most MHN_CHAT_BURST lines in MHN_CHAT_WINDOW_MS; the rest are dropped. */
    const uint64_t now = net_now_ms();
    int oldest = 0;
    for (int i = 1; i < MHN_CHAT_BURST; i++) if (c->chat_ms[i] < c->chat_ms[oldest]) oldest = i;
    if (c->chat_ms[oldest] && now - c->chat_ms[oldest] < MHN_CHAT_WINDOW_MS) return;
    c->chat_ms[oldest] = now;
    uint8_t f[6 + MHN_NICK_LEN + MHN_CHAT_MAX];
    memcpy(f, c->mac, 6);
    memcpy(f + 6, c->nick, MHN_NICK_LEN);
    memcpy(f + 6 + MHN_NICK_LEN, b, len);
    for (int i = 0; i < h->ncl; i++) {
        client *o = h->cl[i];
        if (o != c && !o->dead && o->room == c->room) send_frame(o, MHN_CHAT, f, 6 + MHN_NICK_LEN + len);
    }
}

/* ---- the room's frame rate: the slowest member's ---- */

/* The slowest member's frame rate (60 for an empty room). */
static int room_fps_of(mhn_hub *h, int r, const client **slow) {
    int fps = 60;
    for (int i = 0; i < h->ncl; i++) {
        const client *c = h->cl[i];
        if (c->dead || c->room != r) continue;
        if (c->fps < fps) { fps = c->fps; if (slow) *slow = c; }
    }
    return fps;
}

static void room_fps(mhn_hub *h, int r) {
    if (r < 0) return;
    const client *slow = NULL;
    const int fps = room_fps_of(h, r, &slow);
    uint8_t b[1 + MHN_NICK_LEN];
    memset(b, 0, sizeof b);
    b[0] = (uint8_t)fps;
    if (slow) memcpy(b + 1, slow->nick, MHN_NICK_LEN);
    for (int i = 0; i < h->ncl; i++) {
        client *c = h->cl[i];
        if (!c->dead && c->room == r) send_frame(c, MHN_ROOM_FPS, b, sizeof b);
    }
}

/* ---- the session ---- */

static void hello(mhn_hub *h, client *c, const uint8_t *b, uint32_t len) {
    uint8_t w[4 + MHN_CODE_LEN];
    memset(w, 0, sizeof w);
    if (len < 4 + 1 + MHN_CODE_LEN + 6 + MHN_NICK_LEN || mhn_get32(b) != MHN_MAGIC) {
        mhn_put32(w, MHN_BAD_VERSION);
        send_frame(c, MHN_WELCOME, w, sizeof w);
        c->dead = 1;
        return;
    }
    int create = b[4];
    char code[MHN_CODE_LEN], pass[MHN_PASS_LEN] = "";
    memcpy(code, b + 5, MHN_CODE_LEN);
    code[MHN_CODE_LEN - 1] = 0;
    const uint32_t pass_at = 5 + MHN_CODE_LEN + 6 + MHN_NICK_LEN;
    if (len >= pass_at + MHN_PASS_LEN) {
        memcpy(pass, b + pass_at, MHN_PASS_LEN);
        pass[MHN_PASS_LEN - 1] = 0;
    }
    memcpy(c->mac, b + 5 + MHN_CODE_LEN, 6);
    memcpy(c->nick, b + 5 + MHN_CODE_LEN + 6, MHN_NICK_LEN);
    c->nick[MHN_NICK_LEN - 1] = 0;

    int r = -1;
    if (create && h->open_rooms) {
        if (h->lim.rooms_per_ip && rooms_open(h, c->ip) >= h->lim.rooms_per_ip) {
            logf_("hub: %s may not open more rooms (%d open)", c->ip, h->lim.rooms_per_ip);
            mhn_put32(w, MHN_ROOM_FULL);
            send_frame(c, MHN_WELCOME, w, sizeof w);
            c->dead = 1;
            return;
        }
        r = new_room(h);
        if (r >= 0) {
            room_label(&h->rooms[r], pass, c->nick);
            snprintf(h->rooms[r].by_ip, sizeof h->rooms[r].by_ip, "%s", c->ip);
        }
    } else {
        r = find_room(h, code);
    }
    if (r < 0) { mhn_put32(w, MHN_NO_ROOM); send_frame(c, MHN_WELCOME, w, sizeof w); c->dead = 1; return; }
    if (!create && h->rooms[r].pass[0] && strcmp(h->rooms[r].pass, pass) != 0) {
        mhn_put32(w, MHN_BAD_PASSCODE);
        send_frame(c, MHN_WELCOME, w, sizeof w);
        c->dead = 1;
        return;
    }
    int members = 0;
    for (int i = 0; i < h->ncl; i++) {
        const client *o = h->cl[i];
        if (o == c || o->dead || o->room != r) continue;
        members++;
        if (memcmp(o->mac, c->mac, 6) == 0) {
            mhn_put32(w, MHN_MAC_IN_USE);
            send_frame(c, MHN_WELCOME, w, sizeof w);
            c->dead = 1;
            return;
        }
    }
    if (members >= MHN_MAX_MEMBERS) { mhn_put32(w, MHN_ROOM_FULL); send_frame(c, MHN_WELCOME, w, sizeof w); c->dead = 1; return; }

    c->room = r;
    h->rooms[r].empty_since = 0;
    mhn_put32(w, MHN_OK);
    memcpy(w + 4, h->rooms[r].code, MHN_CODE_LEN);
    send_frame(c, MHN_WELCOME, w, sizeof w);
    for (int i = 0; i < h->ncl; i++) {
        client *o = h->cl[i];
        if (o == c || o->dead || o->room != r) continue;
        send_member(c, MHN_MEMBER, 1, o);
        send_member(o, MHN_MEMBER, 1, c);
    }
    char ms[18];
    mac_str(c->mac, ms);
    logf_("hub: %s (%s, %s) is in room %s, %d member(s)", c->nick, ms, c->ip, h->rooms[r].code, members + 1);
    room_fps(h, r);
}

static void send_list(mhn_hub *h, client *c) {
    static uint8_t b[4 + MHN_LIST_MAX * (MHN_ENTRY_LEN + 1)];
    uint8_t fps[MHN_LIST_MAX];
    const int n = list_entries(h, b + 4, fps, MHN_LIST_MAX);
    mhn_put32(b, (uint32_t)n);
    memcpy(b + 4 + n * MHN_ENTRY_LEN, fps, (size_t)n);              /* the frame rates, after the entries */
    send_frame(c, MHN_ROOM_LIST, b, 4 + (uint32_t)n * (MHN_ENTRY_LEN + 1));
}

static void handle(mhn_hub *h, client *c, int type, uint8_t *b, uint32_t len) {
    if (c->room < 0 && type == MHN_LIST) { send_list(h, c); return; }
    if (c->room < 0 && type != MHN_HELLO) { c->dead = 1; return; }
    switch (type) {
    case MHN_HELLO:         if (c->room < 0) hello(h, c, b, len); break;
    case MHN_CTL_CONNECT:   if (len >= MHN_GROUP_LEN) join_group(h, c, (const char *)b); break;
    case MHN_CTL_DISCONNECT: leave_group(h, c); break;
    case MHN_CTL_SCAN:      send_scan(h, c); break;
    case MHN_PDP: case MHN_PTP_SYN: case MHN_PTP_ACK: case MHN_PTP_RST:
    case MHN_PTP_DATA: case MHN_PTP_FIN:
        route(h, c, type, b, len);
        break;
    case MHN_CHAT:          chat(h, c, b, len); break;
    case MHN_FPS:
        if (len >= 1) { c->fps = b[0] >= 60 ? 60 : 30; room_fps(h, c->room); }
        break;
    case MHN_PING:          if (len >= 8) send_frame(c, MHN_PONG, b, len > 16 ? 16 : len); break;
    default:                break;
    }
}

static void drop(mhn_hub *h, int i) {
    client *c = h->cl[i];
    if (c->room >= 0) {
        leave_group(h, c);
        int left = 0;
        for (int k = 0; k < h->ncl; k++) {
            client *o = h->cl[k];
            if (o == c || o->dead || o->room != c->room) continue;
            send_member(o, MHN_MEMBER, 0, c);
            left++;
        }
        if (!left) h->rooms[c->room].empty_since = net_now_ms();
        logf_("hub: %s left room %s", c->nick, h->rooms[c->room].code);
        c->dead = 1;                                  /* not counted any more */
        room_fps(h, c->room);
    }
    net_close(c->s);
    free(c->buf);
    free(c);
    h->cl[i] = h->cl[--h->ncl];
}

static void read_client(mhn_hub *h, client *c) {
    if (c->cap - c->len < 16384) {
        uint32_t cap = c->cap ? c->cap * 2 : 32768;
        if (cap > MHN_MAX_FRAME * 2 + 16384) { c->dead = 1; return; }
        uint8_t *nb = (uint8_t *)realloc(c->buf, cap);
        if (!nb) { c->dead = 1; return; }
        c->buf = nb;
        c->cap = cap;
    }
    int n = net_recv(c->s, c->buf + c->len, c->cap - c->len);
    if (n <= 0) { c->dead = 1; return; }
    c->len += (uint32_t)n;
    c->last_ms = net_now_ms();
    c->win_bytes += (uint64_t)n;
    h->win_bytes += (uint64_t)n;
    uint32_t off = 0;
    while (c->len - off >= 5) {
        uint32_t fl = mhn_get32(c->buf + off);
        if (fl == 0 || fl > MHN_MAX_FRAME) { c->dead = 1; return; }
        if (c->len - off < 4 + fl) break;
        handle(h, c, c->buf[off + 4], c->buf + off + 5, fl - 1);
        off += 4 + fl;
        if (c->dead) return;
    }
    memmove(c->buf, c->buf + off, c->len - off);
    c->len -= off;
}

static void lan_query(mhn_hub *h) {
    uint8_t b[64];
    uint32_t ip;
    uint16_t port;
    int n = net_udp_recvfrom(h->usock, b, sizeof b, &ip, &port);
    if (n >= 4 && memcmp(b, "MHNL", 4) == 0) {             /* who has rooms here? */
        static uint8_t r[4 + 2 + 1 + 16 * (MHN_ENTRY_LEN + 1)];
        uint8_t fps[16];
        memcpy(r, "MHNR", 4);
        mhn_put16(r + 4, h->port);
        const int k = list_entries(h, r + 7, fps, 16);
        r[6] = (uint8_t)k;
        memcpy(r + 7 + k * MHN_ENTRY_LEN, fps, (size_t)k);           /* the frame rates, after the entries */
        if (k) net_udp_sendto(h->usock, ip, port, r, 7 + (size_t)k * (MHN_ENTRY_LEN + 1));
        return;
    }
    if (n < 4 + MHN_CODE_LEN || memcmp(b, "MHN?", 4) != 0) return;
    char code[MHN_CODE_LEN];
    memcpy(code, b + 4, MHN_CODE_LEN);
    code[MHN_CODE_LEN - 1] = 0;
    if (find_room(h, code) < 0) return;
    uint8_t r[4 + MHN_CODE_LEN + 2];
    memcpy(r, "MHN!", 4);
    memcpy(r + 4, code, MHN_CODE_LEN);
    mhn_put16(r + 4 + MHN_CODE_LEN, h->port);
    net_udp_sendto(h->usock, ip, port, r, sizeof r);
}

/* Over its share this second? Then it is not read until the next one: TCP
 * holds the rest back at the sender, nothing is lost and nothing spins. */
static int throttled(mhn_hub *h, client *c, uint64_t now) {
    if (now - c->win_ms >= 1000) { c->win_ms = now; c->win_bytes = 0; }
    if (h->lim.kbps_per_client && c->win_bytes >= (uint64_t)h->lim.kbps_per_client * 1024) return 1;
    if (h->lim.kbps_total && h->win_bytes >= (uint64_t)h->lim.kbps_total * 1024) return 1;
    return 0;
}

static void note(mhn_hub *h, uint64_t now, const char *fmt, const char *ip) {
    if (now < h->next_note_ms) return;
    h->next_note_ms = now + 1000;
    logf_(fmt, ip);
}

static void accept_client(mhn_hub *h) {
    char ip[48] = "";
    net_sock s = net_tcp_accept(h->lsock, ip, sizeof ip);
    if (s == NET_BAD_SOCK) return;
    const uint64_t now = net_now_ms();
    const int cap = h->lim.max_clients && h->lim.max_clients < MAX_CLIENTS ? h->lim.max_clients : MAX_CLIENTS;
    if (h->ncl >= cap) { note(h, now, "hub: full, refused %s", ip); net_close(s); return; }
    if (h->lim.max_per_ip) {
        int same = 0;
        for (int i = 0; i < h->ncl; i++) same += !strcmp(h->cl[i]->ip, ip);
        if (same >= h->lim.max_per_ip) { note(h, now, "hub: too many connections from %s", ip); net_close(s); return; }
    }
    client *c = (client *)calloc(1, sizeof *c);
    if (!c) { net_close(s); return; }
    if (h->lim.send_timeout_ms) net_set_send_timeout(s, h->lim.send_timeout_ms);
    c->s = s;
    c->room = -1;
    c->fps = 30;
    c->last_ms = c->since_ms = c->win_ms = now;
    snprintf(c->ip, sizeof c->ip, "%s", ip);
    h->cl[h->ncl++] = c;
}

void mhn_hub_poll(mhn_hub *h, int timeout_ms) {
    net_sock socks[NET_POLL_MAX];
    uint8_t ready[NET_POLL_MAX];
    client *who[NET_POLL_MAX];
    uint64_t now = net_now_ms();
    if (now - h->win_ms >= 1000) { h->win_ms = now; h->win_bytes = 0; }
    int n = 0;
    socks[n] = h->lsock; who[n++] = NULL;
    socks[n] = h->usock; who[n++] = NULL;
    for (int i = 0; i < h->ncl && n < NET_POLL_MAX; i++) {
        if (throttled(h, h->cl[i], now)) continue;
        socks[n] = h->cl[i]->s;
        who[n++] = h->cl[i];
    }
    if (net_poll(socks, n, ready, timeout_ms) < 0) return;

    for (int i = 2; i < n; i++) if (ready[i] && !who[i]->dead) read_client(h, who[i]);   /* new clients: next pass */
    if (ready[1]) lan_query(h);
    if (ready[0]) accept_client(h);

    now = net_now_ms();
    for (int i = h->ncl - 1; i >= 0; i--) {
        client *c = h->cl[i];
        if (!c->dead && now - c->last_ms > IDLE_DROP_MS && !throttled(h, c, now)) c->dead = 1;
        if (!c->dead && c->room < 0 && h->lim.hello_ms && now - c->since_ms > (uint64_t)h->lim.hello_ms) c->dead = 1;
        if (c->dead) drop(h, i);
    }
    if (h->open_rooms)
        for (int r = 0; r < MAX_ROOMS; r++)
            if (h->rooms[r].used && h->rooms[r].empty_since && now - h->rooms[r].empty_since > ROOM_KEEP_MS) {
                logf_("hub: room %s closed (empty)", h->rooms[r].code);
                h->rooms[r].used = 0;
            }
}

mhn_hub *mhn_hub_new(uint16_t port, int lan, int open_rooms) {
    if (net_sys_init() != 0) return NULL;
    mhn_hub *h = (mhn_hub *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->port = port;
    h->lsock = net_tcp_listen(&h->port);
    if (h->lsock == NET_BAD_SOCK) { free(h); return NULL; }
    h->usock = lan ? net_udp_bind(MHN_PORT) : NET_BAD_SOCK;
    if (lan && h->usock == NET_BAD_SOCK)
        logf_("hub: LAN discovery port %u is busy; joining by code on this network will not work", MHN_PORT);
    h->open_rooms = open_rooms;
    return h;
}

uint16_t mhn_hub_port(const mhn_hub *h) { return h->port; }

void mhn_hub_run(mhn_hub *h) {
    h->running = 1;
    while (!h->stop) mhn_hub_poll(h, 200);
    h->running = 0;
}

static void hub_thread(void *p) { mhn_hub_run((mhn_hub *)p); }

int mhn_hub_start_thread(mhn_hub *h) {
    h->stop = 0;
    h->running = 1;
    if (net_thread_start(hub_thread, h) != 0) { h->running = 0; return -1; }
    return 0;
}

void mhn_hub_stop(mhn_hub *h) {
    h->stop = 1;
    for (int i = 0; i < 50 && h->running; i++) net_sleep_ms(20);
}

void mhn_hub_free(mhn_hub *h) {
    if (!h) return;
    while (h->ncl) { h->cl[0]->room = -1; drop(h, 0); }
    net_close(h->lsock);
    net_close(h->usock);
    free(h);
}
