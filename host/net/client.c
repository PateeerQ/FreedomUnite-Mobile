/* MHFU-Android -- see client.h.
 *
 * A reader thread takes frames off the hub connection and files them under
 * one lock: datagrams into their PDP socket's queue, stream bytes into their
 * PTP connection, group news into the adhocctl state and its event queue. The
 * game's threads only ever look at that state and send. */
#include "client.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PEERS       MHN_MAX_MEMBERS
#define MAX_EVENTS      32
#define PDP_QUEUE_MAX   (256 * 1024)   /* bytes held per PDP socket before dropping */
#define PTP_BUF_MAX     (1024 * 1024)
#define PTP_BACKLOG_MAX 16
#define PING_MS         10000

typedef struct dgram {
    struct dgram *next;
    uint64_t at_ms;              /* when it arrived, for mhn_pdp_stats */
    uint8_t mac[6];
    uint16_t port;
    int len;
    uint8_t data[];
} dgram;

typedef struct {
    uint8_t mac[6];
    uint16_t port;
} pending;

typedef struct {
    int kind;                    /* 0 free, 1 PDP, 2 PTP */
    uint16_t lport;
    /* PDP */
    dgram *head, *tail;
    int queued;
    int limit;                   /* bytes held before the oldest are dropped (the PSP's bufsize) */
    /* PTP */
    int state, peer_closed, refused;
    uint8_t pmac[6];
    uint16_t pport;
    uint8_t *rbuf;
    uint32_t rlen, rcap;
    pending backlog[PTP_BACKLOG_MAX];
    int nbacklog, max_backlog;
} sock;

static struct {
    net_mutex *lock, *send_lock;
    net_sock s;
    volatile int online, stop, reader_running;
    char code[MHN_CODE_LEN];
    uint8_t mac[6];

    mhn_peer members[MAX_PEERS];
    int nmembers;

    int in_group;
    char group[MHN_GROUP_LEN];
    uint8_t bssid[6];
    mhn_peer peers[MAX_PEERS];
    int npeers;
    mhn_scan_entry scan[MAX_PEERS];
    int nscan;
    int ev[MAX_EVENTS][2];
    int nev;

    sock sk[MHN_MAX_SOCKETS + 1];
    uint16_t next_port;
} g;

/* Datagrams in, read by the game, dropped; and how long the read ones waited. */
static uint64_t g_pdp_in, g_pdp_read, g_pdp_dropped, g_pdp_wait_ms, g_pdp_wait_max;

static void ensure_locks(void) {
    if (!g.lock) g.lock = net_mutex_new();
    if (!g.send_lock) g.send_lock = net_mutex_new();
}

static void push_event(int event, int error) {
    if (g.nev == MAX_EVENTS) return;
    g.ev[g.nev][0] = event;
    g.ev[g.nev][1] = error;
    g.nev++;
}

static int send_frame(int type, const void *body, uint32_t len) {
    if (!g.online && type != MHN_HELLO) return -1;
    uint8_t stackbuf[2048];
    uint8_t *f = len + 5 <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(len + 5);
    if (!f) return -1;
    mhn_put32(f, len + 1);
    f[4] = (uint8_t)type;
    if (len) memcpy(f + 5, body, len);
    net_lock(g.send_lock);
    int r = net_send_all(g.s, f, len + 5);
    net_unlock(g.send_lock);
    if (f != stackbuf) free(f);
    return r;
}

static void send_addr(int type, uint16_t sport, const uint8_t dmac[6], uint16_t dport,
                      const void *data, int len) {
    uint8_t stackbuf[2048];
    uint32_t total = MHN_ADDR_LEN + (uint32_t)(len > 0 ? len : 0);
    uint8_t *b = total <= sizeof stackbuf ? stackbuf : (uint8_t *)malloc(total);
    if (!b) return;
    memcpy(b, g.mac, 6);
    mhn_put16(b + 6, sport);
    memcpy(b + 8, dmac, 6);
    mhn_put16(b + 14, dport);
    if (len > 0) memcpy(b + MHN_ADDR_LEN, data, (size_t)len);
    send_frame(type, b, total);
    if (b != stackbuf) free(b);
}

/* ---- incoming ---- */

static void peer_update(mhn_peer *list, int *n, int joined, const uint8_t *mac, const char *nick) {
    for (int i = 0; i < *n; i++) {
        if (memcmp(list[i].mac, mac, 6) != 0) continue;
        if (joined) { memcpy(list[i].nick, nick, MHN_NICK_LEN); list[i].last_ms = net_now_ms(); }
        else list[i] = list[--*n];
        return;
    }
    if (joined && *n < MAX_PEERS) {
        memcpy(list[*n].mac, mac, 6);
        memcpy(list[*n].nick, nick, MHN_NICK_LEN);
        list[*n].nick[MHN_NICK_LEN - 1] = 0;
        list[*n].last_ms = net_now_ms();
        (*n)++;
    }
}

static void touch_peer(const uint8_t *mac) {
    for (int i = 0; i < g.npeers; i++)
        if (memcmp(g.peers[i].mac, mac, 6) == 0) g.peers[i].last_ms = net_now_ms();
}

static sock *find_ptp(uint16_t lport, const uint8_t *pmac, uint16_t pport, int state) {
    for (int i = 1; i <= MHN_MAX_SOCKETS; i++) {
        sock *s = &g.sk[i];
        if (s->kind == 2 && s->state == state && s->lport == lport &&
            memcmp(s->pmac, pmac, 6) == 0 && s->pport == pport)
            return s;
    }
    return NULL;
}

static void on_traffic(int type, const uint8_t *b, uint32_t len) {
    if (len < MHN_ADDR_LEN) return;
    const uint8_t *src = b;
    uint16_t sport = mhn_get16(b + 6), dport = mhn_get16(b + 14);
    const uint8_t *data = b + MHN_ADDR_LEN;
    int dlen = (int)(len - MHN_ADDR_LEN);
    touch_peer(src);

    if (type == MHN_PDP) {
        for (int i = 1; i <= MHN_MAX_SOCKETS; i++) {
            sock *s = &g.sk[i];
            if (s->kind != 1 || s->lport != dport) continue;
            /* Full: the PSP drops what its receive buffer cannot hold. The
             * oldest go, so a game that reads one datagram a frame while its
             * peer sends more sees the newest state, not a queue that grows
             * (with no limit it did: other hunters moved later and later). */
            const int limit = s->limit > 0 ? s->limit : PDP_QUEUE_MAX;
            while (s->head && s->queued + dlen > limit) {
                dgram *old = s->head;
                s->head = old->next;
                if (!s->head) s->tail = NULL;
                s->queued -= old->len;
                g_pdp_dropped++;
                free(old);
            }
            if (dlen > limit) { g_pdp_dropped++; return; }
            dgram *d = (dgram *)malloc(sizeof *d + (size_t)dlen);
            if (!d) return;
            d->next = NULL;
            d->at_ms = net_now_ms();
            g_pdp_in++;
            memcpy(d->mac, src, 6);
            d->port = sport;
            d->len = dlen;
            memcpy(d->data, data, (size_t)dlen);
            if (s->tail) s->tail->next = d; else s->head = d;
            s->tail = d;
            s->queued += dlen;
            return;
        }
        return;
    }

    if (type == MHN_PTP_SYN) {
        for (int i = 1; i <= MHN_MAX_SOCKETS; i++) {
            sock *s = &g.sk[i];
            if (s->kind != 2 || s->state != MHN_PTP_LISTEN || s->lport != dport) continue;
            for (int k = 0; k < s->nbacklog; k++)
                if (memcmp(s->backlog[k].mac, src, 6) == 0 && s->backlog[k].port == sport) return;
            if (s->nbacklog < s->max_backlog) {
                memcpy(s->backlog[s->nbacklog].mac, src, 6);
                s->backlog[s->nbacklog].port = sport;
                s->nbacklog++;
                return;
            }
            break;
        }
        send_addr(MHN_PTP_RST, dport, src, sport, NULL, 0);
        return;
    }

    if (type == MHN_PTP_ACK) {
        sock *s = find_ptp(dport, src, sport, MHN_PTP_SYN_SENT);
        if (s) { s->state = MHN_PTP_ESTABLISHED; s->refused = 0; }
        return;
    }

    sock *s = find_ptp(dport, src, sport, MHN_PTP_ESTABLISHED);
    if (!s) s = find_ptp(dport, src, sport, MHN_PTP_SYN_SENT);
    if (!s) return;
    if (type == MHN_PTP_RST) {
        if (s->state == MHN_PTP_SYN_SENT) { s->state = MHN_PTP_CLOSED; s->refused = 1; }
        else s->peer_closed = 1;
    } else if (type == MHN_PTP_FIN) {
        s->peer_closed = 1;
    } else if (type == MHN_PTP_DATA && s->state == MHN_PTP_ESTABLISHED) {
        if (s->rlen + (uint32_t)dlen > s->rcap) {
            uint32_t cap = s->rcap ? s->rcap : 16384;
            while (cap < s->rlen + (uint32_t)dlen) cap *= 2;
            if (cap > PTP_BUF_MAX) { s->peer_closed = 1; return; }   /* a peer that never reads */
            uint8_t *nb = (uint8_t *)realloc(s->rbuf, cap);
            if (!nb) return;
            s->rbuf = nb;
            s->rcap = cap;
        }
        memcpy(s->rbuf + s->rlen, data, (size_t)dlen);
        s->rlen += (uint32_t)dlen;
    }
}

static void on_frame(int type, const uint8_t *b, uint32_t len) {
    switch (type) {
    case MHN_MEMBER:
        if (len >= 7 + MHN_NICK_LEN) peer_update(g.members, &g.nmembers, b[0], b + 1, (const char *)b + 7);
        break;
    case MHN_CTL_CONNECTED:
        if (len < MHN_GROUP_LEN + 6) break;
        memcpy(g.group, b, MHN_GROUP_LEN);
        memcpy(g.bssid, b + MHN_GROUP_LEN, 6);
        g.in_group = 1;
        g.npeers = 0;
        push_event(MHN_EV_CONNECT, 0);
        break;
    case MHN_CTL_PEER:
        if (len >= 7 + MHN_NICK_LEN) peer_update(g.peers, &g.npeers, b[0], b + 1, (const char *)b + 7);
        break;
    case MHN_CTL_SCAN_RESULT: {
        if (len < 4) break;
        uint32_t n = mhn_get32(b);
        g.nscan = 0;
        for (uint32_t i = 0; i < n && g.nscan < MAX_PEERS && 4 + (i + 1) * (MHN_GROUP_LEN + 6) <= len; i++) {
            memcpy(g.scan[g.nscan].group, b + 4 + i * (MHN_GROUP_LEN + 6), MHN_GROUP_LEN);
            memcpy(g.scan[g.nscan].bssid, b + 4 + i * (MHN_GROUP_LEN + 6) + MHN_GROUP_LEN, 6);
            g.nscan++;
        }
        push_event(MHN_EV_SCAN, 0);
        break;
    }
    case MHN_PDP: case MHN_PTP_SYN: case MHN_PTP_ACK: case MHN_PTP_RST:
    case MHN_PTP_DATA: case MHN_PTP_FIN:
        on_traffic(type, b, len);
        break;
    default:
        break;
    }
}

static void reader(void *arg) {
    (void)arg;
    uint8_t *buf = (uint8_t *)malloc(MHN_MAX_FRAME * 2 + 16384);
    uint32_t len = 0, cap = MHN_MAX_FRAME * 2 + 16384;
    uint64_t last_ping = net_now_ms();
    g.reader_running = 1;
    while (buf && !g.stop) {
        uint8_t ready;
        int k = net_poll(&g.s, 1, &ready, 200);
        if (net_now_ms() - last_ping > PING_MS) { send_frame(MHN_PING, NULL, 0); last_ping = net_now_ms(); }
        if (k <= 0) { if (k < 0) break; continue; }
        int n = net_recv(g.s, buf + len, cap - len);
        if (n <= 0) break;
        len += (uint32_t)n;
        uint32_t off = 0;
        net_lock(g.lock);
        while (len - off >= 5) {
            uint32_t fl = mhn_get32(buf + off);
            if (fl == 0 || fl > MHN_MAX_FRAME) { off = len; break; }
            if (len - off < 4 + fl) break;
            on_frame(buf[off + 4], buf + off + 5, fl - 1);
            off += 4 + fl;
        }
        net_unlock(g.lock);
        memmove(buf, buf + off, len - off);
        len -= off;
    }
    free(buf);
    net_lock(g.lock);
    if (g.online && !g.stop) {
        fprintf(stderr, "net: lost the connection to the hub\n");
        if (g.in_group) push_event(MHN_EV_DISCONNECT, 0);
    }
    g.online = 0;
    g.in_group = 0;
    g.npeers = 0;
    g.nmembers = 0;
    for (int i = 1; i <= MHN_MAX_SOCKETS; i++)
        if (g.sk[i].kind == 2 && g.sk[i].state == MHN_PTP_ESTABLISHED) g.sk[i].peer_closed = 1;
    net_unlock(g.lock);
    g.reader_running = 0;
}

/* ---- the room ---- */

int mhn_client_start(const char *host, uint16_t port, int create, const char *code,
                     const uint8_t mac[6], const char *nick, const char *pass) {
    ensure_locks();
    mhn_client_stop();
    if (net_sys_init() != 0) return -1;
    net_sock s = net_tcp_connect(host, port, 4000);
    if (s == NET_BAD_SOCK) return -1;

    uint8_t h[4 + 1 + MHN_CODE_LEN + 6 + MHN_NICK_LEN + MHN_PASS_LEN];
    memset(h, 0, sizeof h);
    mhn_put32(h, MHN_MAGIC);
    h[4] = (uint8_t)(create ? 1 : 0);
    if (code) strncpy((char *)h + 5, code, MHN_CODE_LEN - 1);
    memcpy(h + 5 + MHN_CODE_LEN, mac, 6);
    if (nick) strncpy((char *)h + 5 + MHN_CODE_LEN + 6, nick, MHN_NICK_LEN - 1);
    if (pass) strncpy((char *)h + 5 + MHN_CODE_LEN + 6 + MHN_NICK_LEN, pass, MHN_PASS_LEN - 1);
    g.s = s;
    memcpy(g.mac, mac, 6);
    if (send_frame(MHN_HELLO, h, sizeof h) != 0) { net_close(s); return -1; }

    /* The WELCOME is the first frame back. */
    uint8_t w[5 + 4 + MHN_CODE_LEN];
    uint32_t got = 0;
    uint64_t until = net_now_ms() + 5000;
    while (got < sizeof w && net_now_ms() < until) {
        uint8_t ready;
        if (net_poll(&s, 1, &ready, 200) <= 0) continue;
        int n = net_recv(s, w + got, sizeof w - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    if (got < sizeof w || w[4] != MHN_WELCOME) { net_close(s); return -1; }
    uint32_t status = mhn_get32(w + 5);
    if (status != MHN_OK) { net_close(s); return (int)status; }

    net_lock(g.lock);
    memcpy(g.code, w + 9, MHN_CODE_LEN);
    g.code[MHN_CODE_LEN - 1] = 0;
    g.nmembers = g.npeers = g.nscan = g.nev = 0;
    g.in_group = 0;
    g.online = 1;
    g.stop = 0;
    net_unlock(g.lock);
    if (net_thread_start(reader, NULL) != 0) { g.online = 0; net_close(s); return -1; }
    return MHN_OK;
}

void mhn_client_stop(void) {
    ensure_locks();
    if (!g.online && !g.reader_running) return;
    g.stop = 1;
    net_close(g.s);
    for (int i = 0; i < 100 && g.reader_running; i++) net_sleep_ms(10);
    net_lock(g.lock);
    g.online = 0;
    g.code[0] = 0;
    net_unlock(g.lock);
}

int mhn_client_online(void) { return g.online; }

void mhn_client_code(char code[MHN_CODE_LEN]) {
    ensure_locks();
    net_lock(g.lock);
    memcpy(code, g.online ? g.code : "", g.online ? MHN_CODE_LEN : 1);
    net_unlock(g.lock);
}

void mhn_client_mac(uint8_t mac[6]) { memcpy(mac, g.mac, 6); }

int mhn_client_members(mhn_peer *out, int max) {
    ensure_locks();
    net_lock(g.lock);
    int n = g.nmembers < max ? g.nmembers : max;
    memcpy(out, g.members, (size_t)n * sizeof *out);
    net_unlock(g.lock);
    return n;
}

int mhn_lan_find(const char *code, char *host, int host_cap, uint16_t *port, int timeout_ms) {
    if (net_sys_init() != 0) return -1;
    net_sock u = net_udp_bind(0);
    if (u == NET_BAD_SOCK) return -1;
    uint8_t q[4 + MHN_CODE_LEN];
    memcpy(q, "MHN?", 4);
    memset(q + 4, 0, MHN_CODE_LEN);
    strncpy((char *)q + 4, code, MHN_CODE_LEN - 1);
    uint64_t until = net_now_ms() + (uint64_t)timeout_ms, next_send = 0;
    int found = -1;
    while (found != 0 && net_now_ms() < until) {
        if (net_now_ms() >= next_send) {
            net_udp_broadcast(u, MHN_PORT, q, sizeof q);
            net_udp_sendto(u, 0x7F000001u, MHN_PORT, q, sizeof q);   /* a hub on this machine */
            next_send = net_now_ms() + 300;
        }
        uint8_t ready;
        if (net_poll(&u, 1, &ready, 100) <= 0) continue;
        uint8_t r[64];
        uint32_t ip;
        int n = net_udp_recvfrom(u, r, sizeof r, &ip, NULL);
        if (n >= 4 + MHN_CODE_LEN + 2 && memcmp(r, "MHN!", 4) == 0 &&
            strncmp((const char *)r + 4, code, MHN_CODE_LEN) == 0) {
            net_ip_str(ip, host, (size_t)host_cap);
            *port = mhn_get16(r + 4 + MHN_CODE_LEN);
            found = 0;
        }
    }
    net_close(u);
    return found;
}

/* ---- room lists ---- */

static int add_entry(mhn_room_info *out, int n, int max, const uint8_t *e, const char *host, uint16_t port,
                     int relay) {
    char code[MHN_CODE_LEN];
    memcpy(code, e, MHN_CODE_LEN);
    code[MHN_CODE_LEN - 1] = 0;
    for (int i = 0; i < n; i++) if (!strcmp(out[i].code, code)) return n;   /* heard twice */
    if (n >= max) return n;
    mhn_room_info *r = &out[n];
    memset(r, 0, sizeof *r);
    memcpy(r->code, code, MHN_CODE_LEN);
    memcpy(r->name, e + MHN_CODE_LEN, MHN_NAME_LEN);
    r->name[MHN_NAME_LEN - 1] = 0;
    r->players = e[MHN_CODE_LEN + MHN_NAME_LEN];
    r->locked = e[MHN_CODE_LEN + MHN_NAME_LEN + 1];
    snprintf(r->host, sizeof r->host, "%s", host);
    r->port = port;
    r->relay = relay;
    return n + 1;
}

int mhn_lan_list(mhn_room_info *out, int max, int timeout_ms) {
    if (net_sys_init() != 0) return 0;
    net_sock u = net_udp_bind(0);
    if (u == NET_BAD_SOCK) return 0;
    static const uint8_t q[4] = { 'M', 'H', 'N', 'L' };
    uint64_t until = net_now_ms() + (uint64_t)timeout_ms, next_send = 0;
    int n = 0;
    while (net_now_ms() < until) {
        if (net_now_ms() >= next_send) {
            net_udp_broadcast(u, MHN_PORT, q, sizeof q);
            net_udp_sendto(u, 0x7F000001u, MHN_PORT, q, sizeof q);   /* a hub on this machine */
            next_send = net_now_ms() + 300;
        }
        uint8_t ready;
        if (net_poll(&u, 1, &ready, 100) <= 0) continue;
        uint8_t r[7 + 16 * MHN_ENTRY_LEN];
        uint32_t ip;
        const int got = net_udp_recvfrom(u, r, sizeof r, &ip, NULL);
        if (got < 7 || memcmp(r, "MHNR", 4) != 0) continue;
        char host[64];
        net_ip_str(ip, host, sizeof host);
        const uint16_t port = mhn_get16(r + 4);
        for (int k = 0; k < r[6] && 7 + (k + 1) * MHN_ENTRY_LEN <= got; k++)
            n = add_entry(out, n, max, r + 7 + k * MHN_ENTRY_LEN, host, port, 0);
    }
    net_close(u);
    return n;
}

int mhn_hub_list(const char *host, uint16_t port, mhn_room_info *out, int max, int timeout_ms) {
    if (net_sys_init() != 0) return -1;
    net_sock s = net_tcp_connect(host, port, timeout_ms);
    if (s == NET_BAD_SOCK) return -1;
    const uint8_t ask[5] = { 1, 0, 0, 0, MHN_LIST };              /* length 1: just the type */
    if (net_send_all(s, ask, sizeof ask) != 0) { net_close(s); return -1; }
    static uint8_t b[5 + 4 + MHN_LIST_MAX * MHN_ENTRY_LEN];
    uint32_t got = 0, want = 5;
    const uint64_t until = net_now_ms() + (uint64_t)timeout_ms;
    while (got < want && net_now_ms() < until) {
        uint8_t ready;
        if (net_poll(&s, 1, &ready, 100) <= 0) continue;
        const int n = net_recv(s, b + got, want - got);
        if (n <= 0) break;
        got += (uint32_t)n;
        if (got == 5 && want == 5) {
            const uint32_t fl = mhn_get32(b);
            if (b[4] != MHN_ROOM_LIST || fl < 5 || 4 + fl > sizeof b) break;
            want = 4 + fl;
        }
    }
    net_close(s);
    if (got < want || want < 9) return -1;
    const uint32_t count = mhn_get32(b + 5);
    int n = 0;
    for (uint32_t k = 0; k < count && 9 + (k + 1) * MHN_ENTRY_LEN <= got; k++)
        n = add_entry(out, n, max, b + 9 + k * MHN_ENTRY_LEN, host, port, 1);
    return n;
}

/* ---- adhocctl ---- */

void mhn_ctl_connect(const char group[MHN_GROUP_LEN]) { send_frame(MHN_CTL_CONNECT, group, MHN_GROUP_LEN); }

void mhn_ctl_disconnect(void) {
    ensure_locks();
    send_frame(MHN_CTL_DISCONNECT, NULL, 0);
    net_lock(g.lock);
    int was = g.in_group;
    g.in_group = 0;
    g.npeers = 0;
    if (was) push_event(MHN_EV_DISCONNECT, 0);
    net_unlock(g.lock);
}

void mhn_ctl_scan(void) {
    if (send_frame(MHN_CTL_SCAN, NULL, 0) != 0) {
        /* Offline: the scan still completes, with nothing found. */
        ensure_locks();
        net_lock(g.lock);
        g.nscan = 0;
        push_event(MHN_EV_SCAN, 0);
        net_unlock(g.lock);
    }
}

int mhn_ctl_next_event(int *event, int *error) {
    ensure_locks();
    net_lock(g.lock);
    int got = g.nev > 0;
    if (got) {
        *event = g.ev[0][0];
        *error = g.ev[0][1];
        memmove(g.ev[0], g.ev[1], (size_t)(g.nev - 1) * sizeof g.ev[0]);
        g.nev--;
    }
    net_unlock(g.lock);
    return got;
}

int mhn_ctl_group(char group[MHN_GROUP_LEN], uint8_t bssid[6]) {
    ensure_locks();
    net_lock(g.lock);
    int in = g.in_group;
    if (in) {
        if (group) memcpy(group, g.group, MHN_GROUP_LEN);
        if (bssid) memcpy(bssid, g.bssid, 6);
    }
    net_unlock(g.lock);
    return in;
}

int mhn_ctl_scan_results(mhn_scan_entry *out, int max) {
    ensure_locks();
    net_lock(g.lock);
    int n = g.nscan < max ? g.nscan : max;
    memcpy(out, g.scan, (size_t)n * sizeof *out);
    net_unlock(g.lock);
    return n;
}

int mhn_ctl_peers(mhn_peer *out, int max) {
    ensure_locks();
    net_lock(g.lock);
    int n = g.npeers < max ? g.npeers : max;
    memcpy(out, g.peers, (size_t)n * sizeof *out);
    net_unlock(g.lock);
    return n;
}

/* ---- sockets ---- */

static int port_in_use(uint16_t port, int kind) {
    for (int i = 1; i <= MHN_MAX_SOCKETS; i++)
        if (g.sk[i].kind == kind && g.sk[i].lport == port &&
            (kind == 1 || g.sk[i].state == MHN_PTP_LISTEN))
            return 1;
    return 0;
}

static uint16_t pick_port(int kind) {
    if (g.next_port < 50000) g.next_port = 50000;
    for (int tries = 0; tries < 10000; tries++) {
        uint16_t p = g.next_port++;
        if (g.next_port > 60000) g.next_port = 50000;
        if (!port_in_use(p, kind)) return p;
    }
    return 0;
}

static int new_socket(int kind, uint16_t port) {
    for (int i = 1; i <= MHN_MAX_SOCKETS; i++) {
        if (g.sk[i].kind) continue;
        memset(&g.sk[i], 0, sizeof g.sk[i]);
        g.sk[i].kind = kind;
        g.sk[i].lport = port;
        return i;
    }
    return MHN_ERR_NO_SOCKET;
}

static sock *get(int id, int kind) {
    if (id < 1 || id > MHN_MAX_SOCKETS || g.sk[id].kind != kind) return NULL;
    return &g.sk[id];
}

int mhn_socket_kind(int id) {
    if (id < 1 || id > MHN_MAX_SOCKETS) return 0;
    return g.sk[id].kind;
}

int mhn_pdp_create(uint16_t port) {
    ensure_locks();
    net_lock(g.lock);
    int r;
    if (port && port_in_use(port, 1)) r = MHN_ERR_PORT_IN_USE;
    else r = new_socket(1, port ? port : pick_port(1));
    net_unlock(g.lock);
    return r;
}

int mhn_pdp_send(int id, const uint8_t dmac[6], uint16_t dport, const void *data, int len) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 1);
    uint16_t lport = s ? s->lport : 0;
    net_unlock(g.lock);
    if (!s) return MHN_ERR_BAD_ID;
    if (len > MHN_MAX_FRAME - MHN_ADDR_LEN - 16) return MHN_ERR_BAD_ID;
    send_addr(MHN_PDP, lport, dmac, dport, data, len);
    return len;
}

int mhn_pdp_recv(int id, uint8_t smac[6], uint16_t *sport, void *buf, int *len) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 1);
    int r = 1;
    if (!s) r = MHN_ERR_BAD_ID;
    else if (s->head) {
        dgram *d = s->head;
        if (d->len > *len) { *len = d->len; r = 2; }
        else {
            memcpy(buf, d->data, (size_t)d->len);
            *len = d->len;
            if (smac) memcpy(smac, d->mac, 6);
            if (sport) *sport = d->port;
            const uint64_t waited = net_now_ms() - d->at_ms;
            g_pdp_read++;
            g_pdp_wait_ms += waited;
            if (waited > g_pdp_wait_max) g_pdp_wait_max = waited;
            s->head = d->next;
            if (!s->head) s->tail = NULL;
            s->queued -= d->len;
            free(d);
            r = 0;
        }
    }
    net_unlock(g.lock);
    return r;
}

void mhn_pdp_set_limit(int id, int bytes) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 1);
    if (s) s->limit = bytes;
    net_unlock(g.lock);
}

void mhn_pdp_stats(uint64_t *in, uint64_t *read, uint64_t *dropped, uint64_t *wait_ms_total, uint64_t *wait_ms_max) {
    ensure_locks();
    net_lock(g.lock);
    *in = g_pdp_in; *read = g_pdp_read; *dropped = g_pdp_dropped;
    *wait_ms_total = g_pdp_wait_ms; *wait_ms_max = g_pdp_wait_max;
    g_pdp_wait_max = 0;
    net_unlock(g.lock);
}

int mhn_pdp_next_size(int id) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 1);
    int n = s && s->head ? s->head->len : 0;
    net_unlock(g.lock);
    return n;
}

uint16_t mhn_pdp_port(int id) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 1);
    uint16_t p = s ? s->lport : 0;
    net_unlock(g.lock);
    return p;
}

void mhn_pdp_delete(int id) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 1);
    if (s) {
        while (s->head) { dgram *d = s->head; s->head = d->next; free(d); }
        memset(s, 0, sizeof *s);
    }
    net_unlock(g.lock);
}

int mhn_ptp_open(uint16_t sport, const uint8_t dmac[6], uint16_t dport) {
    ensure_locks();
    net_lock(g.lock);
    int id = new_socket(2, sport ? sport : pick_port(2));
    if (id > 0) {
        g.sk[id].state = MHN_PTP_CLOSED;
        memcpy(g.sk[id].pmac, dmac, 6);
        g.sk[id].pport = dport;
    }
    net_unlock(g.lock);
    return id;
}

int mhn_ptp_connect(int id) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 2);
    int st = -1, send = 0;
    uint16_t lport = 0, pport = 0;
    uint8_t pmac[6];
    if (s) {
        if (s->state == MHN_PTP_CLOSED) { s->state = MHN_PTP_SYN_SENT; send = 1; }
        st = s->state;
        lport = s->lport; pport = s->pport;
        memcpy(pmac, s->pmac, 6);
    }
    net_unlock(g.lock);
    if (send) send_addr(MHN_PTP_SYN, lport, pmac, pport, NULL, 0);
    return st;
}

int mhn_ptp_listen(uint16_t sport, int backlog) {
    ensure_locks();
    net_lock(g.lock);
    int id;
    if (sport && port_in_use(sport, 2)) id = MHN_ERR_PORT_IN_USE;
    else {
        id = new_socket(2, sport ? sport : pick_port(2));
        if (id > 0) {
            g.sk[id].state = MHN_PTP_LISTEN;
            g.sk[id].max_backlog = backlog < 1 ? 1 : backlog > PTP_BACKLOG_MAX ? PTP_BACKLOG_MAX : backlog;
        }
    }
    net_unlock(g.lock);
    return id;
}

int mhn_ptp_accept(int listen_id, uint8_t mac[6], uint16_t *port) {
    ensure_locks();
    net_lock(g.lock);
    sock *l = get(listen_id, 2);
    int id = 0;
    pending p;
    uint16_t lport = 0;
    if (!l || l->state != MHN_PTP_LISTEN) id = MHN_ERR_BAD_ID;
    else if (l->nbacklog) {
        p = l->backlog[0];
        lport = l->lport;
        memmove(l->backlog, l->backlog + 1, (size_t)(l->nbacklog - 1) * sizeof p);
        l->nbacklog--;
        id = new_socket(2, lport);
        if (id > 0) {
            g.sk[id].state = MHN_PTP_ESTABLISHED;
            memcpy(g.sk[id].pmac, p.mac, 6);
            g.sk[id].pport = p.port;
            if (mac) memcpy(mac, p.mac, 6);
            if (port) *port = p.port;
        }
    }
    net_unlock(g.lock);
    if (id > 0) send_addr(MHN_PTP_ACK, lport, p.mac, p.port, NULL, 0);
    else if (id == MHN_ERR_NO_SOCKET) { send_addr(MHN_PTP_RST, lport, p.mac, p.port, NULL, 0); id = 0; }
    return id;
}

int mhn_ptp_send(int id, const void *data, int len) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 2);
    int ok = s && s->state == MHN_PTP_ESTABLISHED && !s->peer_closed;
    uint16_t lport = ok ? s->lport : 0, pport = ok ? s->pport : 0;
    uint8_t pmac[6];
    if (ok) memcpy(pmac, s->pmac, 6);
    net_unlock(g.lock);
    if (!ok) return -1;
    /* In pieces, so one large send does not hold the hub on a single frame. */
    const uint8_t *p = (const uint8_t *)data;
    int left = len;
    while (left > 0) {
        int n = left > 16384 ? 16384 : left;
        send_addr(MHN_PTP_DATA, lport, pmac, pport, p, n);
        p += n;
        left -= n;
    }
    return len;
}

int mhn_ptp_recv(int id, void *buf, int *len) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 2);
    int r;
    if (!s) r = -1;
    else if (s->rlen) {
        uint32_t n = (uint32_t)*len < s->rlen ? (uint32_t)*len : s->rlen;
        memcpy(buf, s->rbuf, n);
        memmove(s->rbuf, s->rbuf + n, s->rlen - n);
        s->rlen -= n;
        *len = (int)n;
        r = 0;
    } else r = s->peer_closed || s->state == MHN_PTP_CLOSED ? -1 : 1;
    net_unlock(g.lock);
    return r;
}

static void fill_stat(int id, const sock *s, mhn_ptp_stat *st) {
    st->id = id;
    st->state = s->state;
    memcpy(st->laddr, g.mac, 6);
    memcpy(st->paddr, s->pmac, 6);
    st->lport = s->lport;
    st->pport = s->pport;
    st->rcv_bytes = s->rlen;
    st->peer_closed = s->peer_closed;
    st->backlog = s->nbacklog;
}

int mhn_ptp_stat_of(int id, mhn_ptp_stat *st) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 2);
    if (s) fill_stat(id, s, st);
    net_unlock(g.lock);
    return s ? 0 : -1;
}

int mhn_ptp_list(mhn_ptp_stat *out, int max) {
    ensure_locks();
    net_lock(g.lock);
    int n = 0;
    for (int i = 1; i <= MHN_MAX_SOCKETS && n < max; i++)
        if (g.sk[i].kind == 2) fill_stat(i, &g.sk[i], &out[n++]);
    net_unlock(g.lock);
    return n;
}

void mhn_ptp_close(int id) {
    ensure_locks();
    net_lock(g.lock);
    sock *s = get(id, 2);
    int fin = 0;
    uint16_t lport = 0, pport = 0;
    uint8_t pmac[6];
    if (s) {
        fin = s->state == MHN_PTP_ESTABLISHED && !s->peer_closed;
        lport = s->lport; pport = s->pport;
        memcpy(pmac, s->pmac, 6);
        free(s->rbuf);
        memset(s, 0, sizeof *s);
    }
    net_unlock(g.lock);
    if (fin) send_addr(MHN_PTP_FIN, lport, pmac, pport, NULL, 0);
}
