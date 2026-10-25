/* MHFU-Android -- see adhoc.h.
 *
 * Structure layouts and error codes follow PPSSPP (Core/HLE/proAdhoc.h,
 * sceNetAdhoc.cpp, ErrorCodes.h). The calls that block on a real PSP (PDP and
 * PTP receive, accept, connect, poll) wait here by yielding the guest CPU in
 * 1 ms steps, so the game's other threads keep running, and give up after the
 * game's timeout (0 means none). */
#include "adhoc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include "hle_mhfu.h"
#include "lobby.h"
#include "net/client.h"

/* ---- error codes ---- */
#define ADHOC_INVALID_SOCKET_ID   0x80410701u
#define ADHOC_INVALID_ADDR        0x80410702u
#define ADHOC_INVALID_PORT        0x80410703u
#define ADHOC_INVALID_DATALEN     0x80410705u
#define ADHOC_NOT_ENOUGH_SPACE    0x80400706u
#define ADHOC_WOULD_BLOCK         0x80410709u
#define ADHOC_PORT_IN_USE         0x8041070Au
#define ADHOC_NOT_CONNECTED       0x8041070Bu
#define ADHOC_DISCONNECTED        0x8041070Cu
#define ADHOC_NOT_LISTENED        0x8040070Eu
#define ADHOC_SOCKET_ID_NOT_AVAIL 0x8041070Fu
#define ADHOC_INVALID_ARG         0x80410711u
#define ADHOC_NOT_INITIALIZED     0x80410712u
#define ADHOC_ALREADY_INITIALIZED 0x80410713u
#define ADHOC_TIMEOUT             0x80410715u
#define ADHOC_NO_ENTRY            0x80410716u
#define CTL_ALREADY_CONNECTED     0x80410B02u
#define CTL_INVALID_ARG           0x80410B04u
#define CTL_ALREADY_INITIALIZED   0x80410B07u
#define CTL_NOT_INITIALIZED       0x80410B08u
#define CTL_BUSY                  0x80410B10u

/* adhocctl events and states */
enum { EV_ERROR = 0, EV_CONNECT = 1, EV_DISCONNECT = 2, EV_SCAN = 3 };
enum { ST_DISCONNECTED = 0, ST_CONNECTED = 1, ST_SCANNING = 2 };

/* poll events */
#define EV_SEND        0x0001
#define EV_RECV        0x0002
#define EV_CONNECTED   0x0004
#define EV_ACCEPT      0x0008
#define EV_DISCONNECTED 0x0800

#define SIZE_SCANINFO 28    /* next, channel, group[8], bssid[6]+pad[2], mode */
#define SIZE_PEERINFO 152   /* next, nickname[128], mac[6], pad[2], flags, last_recv(u64) */
#define SIZE_PDPSTAT  20
#define SIZE_PTPSTAT  36
#define MAX_HANDLERS  8
#define MAX_LOCAL_EV  16

static int g_adhoc_inited, g_ctl_inited;
static int g_state;
static int g_local_group;            /* "connected" with no room: alone in the group */
static char g_group[MHN_GROUP_LEN];
static uint8_t g_product[16];
static struct { uint32_t fn, arg; } g_handlers[MAX_HANDLERS];
static int g_local_ev[MAX_LOCAL_EV][2], g_nlocal_ev;

/* ---- helpers ---- */

static void read_mac(uint32_t at, uint8_t mac[6]) { for (int i = 0; i < 6; i++) mac[i] = psp_read8(at + (uint32_t)i); }
static void write_mac(uint32_t at, const uint8_t mac[6]) { for (int i = 0; i < 6; i++) psp_write8(at + (uint32_t)i, mac[i]); }
static void write_bytes(uint32_t at, const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) psp_write8(at + i, b[i]);
}
static void zero(uint32_t at, uint32_t n) { for (uint32_t i = 0; i < n; i++) psp_write8(at + i, 0); }

/* Guest memory as a host pointer, for bulk copies; NULL if out of range. */
static uint8_t *guest(uint32_t at, uint32_t n) {
    return n ? (uint8_t *)psp_mem_ptr(at, n) : NULL;
}

/* The time the game sees from sceKernelGetSystemTimeWide, for last_recv. */
static uint64_t guest_time_us(void) {
    uint32_t v0 = psp_cpu.r[PSP_REG_V0], v1 = psp_cpu.r[PSP_REG_V1];
    psp_hle_call(0x82BC5777u);
    uint64_t t = (uint64_t)psp_cpu.r[PSP_REG_V0] | (uint64_t)psp_cpu.r[PSP_REG_V1] << 32;
    psp_cpu.r[PSP_REG_V0] = v0;
    psp_cpu.r[PSP_REG_V1] = v1;
    return t;
}

/* Wait for `ready` (non-zero) or the timeout. Yields the CPU between looks. */
typedef int (*ready_fn)(int id);
static int wait_for(ready_fn ready, int id, uint32_t timeout_us) {
    uint64_t end = timeout_us ? mhfu_clock_us() + timeout_us : 0;
    for (;;) {
        int r = ready(id);
        if (r) return r;
        if (end && mhfu_clock_us() >= end) return 0;
        psp_threadman_yield();
    }
}

static void post_local(int event, int error) {
    if (g_nlocal_ev < MAX_LOCAL_EV) {
        g_local_ev[g_nlocal_ev][0] = event;
        g_local_ev[g_nlocal_ev][1] = error;
        g_nlocal_ev++;
    }
}

static int online(void) { return mhn_client_online(); }

/* ---- sceNet / sceWlanDrv ---- */

static void hle_NetInit(void)  { psp_ret(0); }
static void hle_NetTerm(void)  { psp_ret(0); }

static void hle_NetGetLocalEtherAddr(void) {
    uint8_t mac[6];
    mhfu_lobby_mac(mac);
    if (psp_arg(0)) write_mac(psp_arg(0), mac);
    psp_ret(0);
}

/* The wireless switch: on. */
static void hle_WlanGetSwitchState(void) { psp_ret(1); }

static void hle_WlanGetEtherAddr(void) { hle_NetGetLocalEtherAddr(); }

/* (u8 psid[16]): games use it to tell consoles apart in ad hoc play, so it
 * follows this device's MAC. */
static void hle_OpenPSIDGetOpenPSID(void) {
    uint8_t id[16] = { 'M', 'H', 'F', 'U', 'R', 'E', 'C', 'O', 'M', 'P', 0, 0, 0, 0, 0, 0 };
    mhfu_lobby_mac(id + 10);
    if (psp_arg(0)) write_bytes(psp_arg(0), id, 16);
    psp_ret(0);
}

/* ---- sceNetAdhoc ---- */

static void hle_AdhocInit(void) {
    if (g_adhoc_inited) { psp_ret(ADHOC_ALREADY_INITIALIZED); return; }
    g_adhoc_inited = 1;
    psp_ret(0);
}

static void hle_AdhocTerm(void) {
    for (int i = 1; i <= MHN_MAX_SOCKETS; i++) {
        if (mhn_socket_kind(i) == 1) mhn_pdp_delete(i);
        else if (mhn_socket_kind(i) == 2) mhn_ptp_close(i);
    }
    g_adhoc_inited = 0;
    psp_ret(0);
}

/* (u8 *mac, u16 port, u32 bufsize, u32 flag) */
static void hle_PdpCreate(void) {
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    int id = mhn_pdp_create((uint16_t)psp_arg(1));
    if (id == MHN_ERR_PORT_IN_USE) { psp_ret(ADHOC_PORT_IN_USE); return; }
    if (id <= 0) { psp_ret(ADHOC_SOCKET_ID_NOT_AVAIL); return; }
    const uint32_t bufsize = psp_arg(2);
    mhn_pdp_set_limit(id, (int)bufsize);
    printf("adhoc: PDP socket %d on port %u, buffer %u bytes\n", id, (unsigned)(uint16_t)psp_arg(1), bufsize);
    psp_ret((uint32_t)id);
}

/* (int id, u8 *mac, u16 port, void *data, int len, u32 timeout, int flag) */
static void hle_PdpSend(void) {
    int id = (int)psp_arg(0);
    uint32_t mac_at = psp_arg(1), data = psp_arg(3);
    uint16_t port = (uint16_t)psp_arg(2);
    int len = (int)psp_arg(4);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    if (mhn_socket_kind(id) != 1) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (!port) { psp_ret(ADHOC_INVALID_PORT); return; }
    if (len < 0 || len > 65523) { psp_ret(ADHOC_INVALID_DATALEN); return; }
    if (!mac_at) { psp_ret(ADHOC_INVALID_ADDR); return; }
    uint8_t mac[6];
    read_mac(mac_at, mac);
    uint8_t *p = guest(data, (uint32_t)len);
    if (len && !p) { psp_ret(ADHOC_INVALID_ARG); return; }
    if (online()) mhn_pdp_send(id, mac, port, p, len);
    psp_ret(0);
}

static int pdp_ready(int id) { return mhn_pdp_next_size(id) > 0 || mhn_socket_kind(id) != 1; }

/* (int id, u8 *mac, u16 *port, void *buf, int *len, u32 timeout, int flag) */
static void hle_PdpRecv(void) {
    int id = (int)psp_arg(0);
    uint32_t mac_at = psp_arg(1), port_at = psp_arg(2), buf = psp_arg(3), len_at = psp_arg(4);
    uint32_t timeout = psp_arg(5);
    int nonblock = (int)psp_arg(6);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    if (mhn_socket_kind(id) != 1) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (!mac_at || !port_at || !len_at) { psp_ret(ADHOC_INVALID_ARG); return; }
    if (!mhn_pdp_next_size(id)) {
        if (nonblock) { psp_ret(ADHOC_WOULD_BLOCK); return; }
        if (!wait_for(pdp_ready, id, timeout)) { psp_ret(ADHOC_TIMEOUT); return; }
    }
    int cap = (int)psp_read32(len_at);
    uint8_t mac[6];
    uint16_t sport;
    uint8_t *p = cap > 0 ? guest(buf, (uint32_t)cap) : NULL;
    static uint8_t scratch[1];
    int len = cap;
    int r = mhn_pdp_recv(id, mac, &sport, p ? p : scratch, &len);
    if (r == 2) { psp_write32(len_at, (uint32_t)len); psp_ret(ADHOC_NOT_ENOUGH_SPACE); return; }
    if (r != 0) { psp_ret(r == 1 ? ADHOC_WOULD_BLOCK : ADHOC_INVALID_SOCKET_ID); return; }
    write_mac(mac_at, mac);
    psp_write16(port_at, sport);
    psp_write32(len_at, (uint32_t)len);
    psp_ret(0);
}

static void hle_PdpDelete(void) {
    int id = (int)psp_arg(0);
    if (mhn_socket_kind(id) != 1) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    mhn_pdp_delete(id);
    psp_ret(0);
}

/* (int *size, SceNetAdhocPdpStat *buf) */
static void hle_GetPdpStat(void) {
    uint32_t size_at = psp_arg(0), buf = psp_arg(1);
    if (!size_at) { psp_ret(ADHOC_INVALID_ARG); return; }
    uint8_t mac[6];
    mhfu_lobby_mac(mac);
    int ids[MHN_MAX_SOCKETS], n = 0;
    for (int i = 1; i <= MHN_MAX_SOCKETS; i++) if (mhn_socket_kind(i) == 1) ids[n++] = i;
    if (!buf) { psp_write32(size_at, (uint32_t)(n * SIZE_PDPSTAT)); psp_ret(0); return; }
    int room = (int)psp_read32(size_at) / SIZE_PDPSTAT, k = 0;
    for (; k < n && k < room; k++) {
        uint32_t at = buf + (uint32_t)k * SIZE_PDPSTAT;
        zero(at, SIZE_PDPSTAT);
        psp_write32(at, k + 1 < n && k + 1 < room ? at + SIZE_PDPSTAT : 0);
        psp_write32(at + 4, (uint32_t)ids[k]);
        write_mac(at + 8, mac);
        psp_write16(at + 14, mhn_pdp_port(ids[k]));
        psp_write32(at + 16, (uint32_t)mhn_pdp_next_size(ids[k]));   /* bytes waiting */
    }
    psp_write32(size_at, (uint32_t)(k * SIZE_PDPSTAT));
    psp_ret(0);
}

/* (u8 *srcmac, u16 sport, u8 *dstmac, u16 dport, u32 bufsize, u32 rexmt_int, int rexmt_cnt, int flag) */
static void hle_PtpOpen(void) {
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    uint32_t dmac_at = psp_arg(2);
    uint16_t dport = (uint16_t)psp_arg(3);
    if (!dmac_at) { psp_ret(ADHOC_INVALID_ADDR); return; }
    if (!dport) { psp_ret(ADHOC_INVALID_PORT); return; }
    uint8_t dmac[6];
    read_mac(dmac_at, dmac);
    int id = mhn_ptp_open((uint16_t)psp_arg(1), dmac, dport);
    psp_ret(id > 0 ? (uint32_t)id : ADHOC_SOCKET_ID_NOT_AVAIL);
}

static int ptp_connected(int id) {
    mhn_ptp_stat st;
    if (mhn_ptp_stat_of(id, &st) != 0) return -1;
    if (st.state == MHN_PTP_ESTABLISHED) return 1;
    if (st.state == MHN_PTP_CLOSED) mhn_ptp_connect(id);   /* refused: knock again */
    return 0;
}

/* (int id, u32 timeout, int flag) */
static void hle_PtpConnect(void) {
    int id = (int)psp_arg(0);
    uint32_t timeout = psp_arg(1);
    int nonblock = (int)psp_arg(2);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    if (mhn_socket_kind(id) != 2) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    int st = mhn_ptp_connect(id);
    if (st == MHN_PTP_ESTABLISHED) { psp_ret(0); return; }
    if (st == MHN_PTP_LISTEN) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (nonblock) { psp_ret(ADHOC_WOULD_BLOCK); return; }
    int r = wait_for(ptp_connected, id, timeout);
    psp_ret(r == 1 ? 0 : r < 0 ? ADHOC_INVALID_SOCKET_ID : ADHOC_TIMEOUT);
}

/* (u8 *srcmac, u16 sport, u32 bufsize, u32 rexmt_int, int rexmt_cnt, int backlog, int flag) */
static void hle_PtpListen(void) {
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    int id = mhn_ptp_listen((uint16_t)psp_arg(1), (int)psp_arg(5));
    if (id == MHN_ERR_PORT_IN_USE) { psp_ret(ADHOC_PORT_IN_USE); return; }
    psp_ret(id > 0 ? (uint32_t)id : ADHOC_SOCKET_ID_NOT_AVAIL);
}

static int g_accept_id;
static uint8_t g_accept_mac[6];
static uint16_t g_accept_port;
static int accept_ready(int id) {
    g_accept_id = mhn_ptp_accept(id, g_accept_mac, &g_accept_port);
    return g_accept_id != 0;
}

/* (int id, u8 *mac, u16 *port, u32 timeout, int flag) */
static void hle_PtpAccept(void) {
    int id = (int)psp_arg(0);
    uint32_t mac_at = psp_arg(1), port_at = psp_arg(2), timeout = psp_arg(3);
    int nonblock = (int)psp_arg(4);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    mhn_ptp_stat st;
    if (mhn_socket_kind(id) != 2 || mhn_ptp_stat_of(id, &st) != 0) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (st.state != MHN_PTP_LISTEN) { psp_ret(ADHOC_NOT_LISTENED); return; }
    int got = accept_ready(id);
    if (!got && nonblock) { psp_ret(ADHOC_WOULD_BLOCK); return; }
    if (!got && !wait_for(accept_ready, id, timeout)) { psp_ret(ADHOC_TIMEOUT); return; }
    if (g_accept_id < 0) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (mac_at) write_mac(mac_at, g_accept_mac);
    if (port_at) psp_write16(port_at, g_accept_port);
    psp_ret((uint32_t)g_accept_id);
}

/* (int id, void *data, int *size, u32 timeout, int flag) */
static void hle_PtpSend(void) {
    int id = (int)psp_arg(0);
    uint32_t data = psp_arg(1), size_at = psp_arg(2);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    if (mhn_socket_kind(id) != 2) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (!size_at) { psp_ret(ADHOC_INVALID_ARG); return; }
    int len = (int)psp_read32(size_at);
    mhn_ptp_stat st;
    mhn_ptp_stat_of(id, &st);
    if (st.state != MHN_PTP_ESTABLISHED) { psp_ret(ADHOC_NOT_CONNECTED); return; }
    uint8_t *p = guest(data, (uint32_t)(len > 0 ? len : 0));
    if (len > 0 && !p) { psp_ret(ADHOC_INVALID_ARG); return; }
    if (mhn_ptp_send(id, p, len) < 0) { psp_ret(ADHOC_DISCONNECTED); return; }
    psp_ret(0);
}

static int ptp_readable(int id) {
    mhn_ptp_stat st;
    if (mhn_ptp_stat_of(id, &st) != 0) return 1;
    return st.rcv_bytes > 0 || st.peer_closed || st.state == MHN_PTP_CLOSED;
}

/* (int id, void *buf, int *size, u32 timeout, int flag) */
static void hle_PtpRecv(void) {
    int id = (int)psp_arg(0);
    uint32_t buf = psp_arg(1), size_at = psp_arg(2), timeout = psp_arg(3);
    int nonblock = (int)psp_arg(4);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    if (mhn_socket_kind(id) != 2) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    if (!size_at || !buf) { psp_ret(ADHOC_INVALID_ARG); return; }
    int cap = (int)psp_read32(size_at);
    uint8_t *p = guest(buf, (uint32_t)(cap > 0 ? cap : 0));
    if (cap <= 0 || !p) { psp_ret(ADHOC_INVALID_ARG); return; }
    if (!ptp_readable(id)) {
        if (nonblock) { psp_ret(ADHOC_WOULD_BLOCK); return; }
        if (!wait_for(ptp_readable, id, timeout)) { psp_ret(ADHOC_TIMEOUT); return; }
    }
    int len = cap;
    int r = mhn_ptp_recv(id, p, &len);
    if (r == 0) { psp_write32(size_at, (uint32_t)len); psp_ret(0); return; }
    psp_ret(r == 1 ? ADHOC_WOULD_BLOCK : ADHOC_DISCONNECTED);
}

static void hle_PtpClose(void) {
    int id = (int)psp_arg(0);
    if (mhn_socket_kind(id) != 2) { psp_ret(ADHOC_INVALID_SOCKET_ID); return; }
    mhn_ptp_close(id);
    psp_ret(0);
}

/* Everything sent is already on its way to the hub. */
static void hle_PtpFlush(void) {
    psp_ret(mhn_socket_kind((int)psp_arg(0)) == 2 ? 0 : ADHOC_INVALID_SOCKET_ID);
}

/* (int *size, SceNetAdhocPtpStat *buf) */
static void hle_GetPtpStat(void) {
    uint32_t size_at = psp_arg(0), buf = psp_arg(1);
    if (!size_at) { psp_ret(ADHOC_INVALID_ARG); return; }
    mhn_ptp_stat st[MHN_MAX_SOCKETS];
    int n = mhn_ptp_list(st, MHN_MAX_SOCKETS);
    if (!buf) { psp_write32(size_at, (uint32_t)(n * SIZE_PTPSTAT)); psp_ret(0); return; }
    int room = (int)psp_read32(size_at) / SIZE_PTPSTAT, k = 0;
    for (; k < n && k < room; k++) {
        uint32_t at = buf + (uint32_t)k * SIZE_PTPSTAT;
        zero(at, SIZE_PTPSTAT);
        psp_write32(at, k + 1 < n && k + 1 < room ? at + SIZE_PTPSTAT : 0);
        psp_write32(at + 4, (uint32_t)st[k].id);
        write_mac(at + 8, st[k].laddr);
        write_mac(at + 14, st[k].paddr);
        psp_write16(at + 20, st[k].lport);
        psp_write16(at + 22, st[k].pport);
        psp_write32(at + 24, 0);                       /* nothing waits to be sent */
        psp_write32(at + 28, st[k].rcv_bytes);
        psp_write32(at + 32, (uint32_t)st[k].state);
    }
    psp_write32(size_at, (uint32_t)(k * SIZE_PTPSTAT));
    psp_ret(0);
}

static uint32_t g_poll_sds;
static int g_poll_count;

static int poll_pass(int unused) {
    (void)unused;
    int affected = 0;
    for (int i = 0; i < g_poll_count; i++) {
        uint32_t at = g_poll_sds + (uint32_t)i * 12;
        int id = (int)psp_read32(at);
        uint32_t want = psp_read32(at + 4), got = 0;
        int kind = mhn_socket_kind(id);
        if (kind == 1) {
            got |= EV_SEND;
            if (mhn_pdp_next_size(id) > 0) got |= EV_RECV;
        } else if (kind == 2) {
            mhn_ptp_stat st;
            mhn_ptp_stat_of(id, &st);
            if (st.state == MHN_PTP_ESTABLISHED) {
                if (!st.peer_closed) got |= EV_SEND | EV_CONNECTED;
                if (st.rcv_bytes) got |= EV_RECV;
                if (st.peer_closed) got |= EV_DISCONNECTED;
            } else if (st.state == MHN_PTP_LISTEN && st.backlog) {
                got |= EV_ACCEPT;
            }
        }
        got &= want | EV_DISCONNECTED;
        psp_write32(at + 8, got);
        if (got) affected++;
    }
    return affected;
}

/* (SceNetAdhocPollSd *sds, int count, u32 timeout, int nonblock) */
static void hle_PollSocket(void) {
    g_poll_sds = psp_arg(0);
    g_poll_count = (int)psp_arg(1);
    uint32_t timeout = psp_arg(2);
    int nonblock = (int)psp_arg(3);
    if (!g_adhoc_inited) { psp_ret(ADHOC_NOT_INITIALIZED); return; }
    if (!g_poll_sds || g_poll_count <= 0) { psp_ret(ADHOC_INVALID_ARG); return; }
    int n = poll_pass(0);
    if (!n && !nonblock && timeout) n = wait_for(poll_pass, 0, timeout);
    psp_ret((uint32_t)n);
}

static void hle_SetSocketAlert(void) { psp_ret(0); }
static void hle_GetSocketAlert(void) { if (psp_arg(1)) psp_write32(psp_arg(1), 0); psp_ret(0); }

/* ---- sceNetAdhocctl ---- */

/* (int stacksize, int prio, SceNetAdhocctlAdhocId *product) */
static void hle_CtlInit(void) {
    if (g_ctl_inited) { psp_ret(CTL_ALREADY_INITIALIZED); return; }
    uint32_t prod = psp_arg(2);
    memset(g_product, 0, sizeof g_product);
    if (prod) for (int i = 0; i < 16; i++) g_product[i] = psp_read8(prod + (uint32_t)i);
    g_ctl_inited = 1;
    g_state = ST_DISCONNECTED;
    g_nlocal_ev = 0;
    { int e, r; while (mhn_ctl_next_event(&e, &r)) {} }
    psp_ret(0);
}

static void hle_CtlTerm(void) {
    if (g_state == ST_CONNECTED && online()) mhn_ctl_disconnect();
    g_ctl_inited = 0;
    g_state = ST_DISCONNECTED;
    g_local_group = 0;
    psp_ret(0);
}

/* (handler, arg) -> handler id */
static void hle_CtlAddHandler(void) {
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (g_handlers[i].fn) continue;
        g_handlers[i].fn = psp_arg(0);
        g_handlers[i].arg = psp_arg(1);
        psp_ret((uint32_t)i);
        return;
    }
    psp_ret(ADHOC_NO_ENTRY);
}

static void hle_CtlDelHandler(void) {
    uint32_t i = psp_arg(0);
    if (i < MAX_HANDLERS) g_handlers[i].fn = 0;
    psp_ret(0);
}

static void connect_group(const char group[MHN_GROUP_LEN]) {
    memcpy(g_group, group, MHN_GROUP_LEN);
    if (online()) mhn_ctl_connect(g_group);
    else { g_local_group = 1; post_local(EV_CONNECT, 0); }
}

static void ctl_connect_common(uint32_t group_at) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (g_state == ST_CONNECTED) { post_local(EV_ERROR, (int)CTL_ALREADY_CONNECTED); psp_ret(0); return; }
    if (g_state == ST_SCANNING) { psp_ret(CTL_BUSY); return; }
    char group[MHN_GROUP_LEN];
    memset(group, 0, sizeof group);
    if (group_at) for (int i = 0; i < MHN_GROUP_LEN; i++) group[i] = (char)psp_read8(group_at + (uint32_t)i);
    connect_group(group);
    psp_ret(0);
}

/* (char *group_name): Create and Connect both mean "be in this group". */
static void hle_CtlCreate(void)  { ctl_connect_common(psp_arg(0)); }
static void hle_CtlConnect(void) { ctl_connect_common(psp_arg(0)); }
/* (SceNetAdhocctlScanInfo *): its group_name is at +8. */
static void hle_CtlJoin(void) {
    uint32_t info = psp_arg(0);
    if (!info) { psp_ret(CTL_INVALID_ARG); return; }
    ctl_connect_common(info + 8);
}

static void hle_CtlDisconnect(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (online() && !g_local_group) mhn_ctl_disconnect();
    else post_local(EV_DISCONNECT, 0);
    g_local_group = 0;
    g_state = ST_DISCONNECTED;
    psp_ret(0);
}

static void hle_CtlScan(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (g_state == ST_CONNECTED) { post_local(EV_ERROR, (int)CTL_ALREADY_CONNECTED); psp_ret(0); return; }
    if (g_state == ST_SCANNING) { psp_ret(CTL_BUSY); return; }
    g_state = ST_SCANNING;
    mhn_ctl_scan();                    /* offline it completes at once with nothing found */
    psp_ret(0);
}

/* (int *size, SceNetAdhocctlScanInfo *buf) */
static void hle_CtlGetScanInfo(void) {
    uint32_t size_at = psp_arg(0), buf = psp_arg(1);
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (!size_at) { psp_ret(CTL_INVALID_ARG); return; }
    mhn_scan_entry s[MHN_MAX_MEMBERS];
    int n = g_state == ST_CONNECTED ? 0 : mhn_ctl_scan_results(s, MHN_MAX_MEMBERS);
    if (!buf) { psp_write32(size_at, (uint32_t)(n * SIZE_SCANINFO)); psp_ret(0); return; }
    int room = (int)psp_read32(size_at) / SIZE_SCANINFO, k = 0;
    for (; k < n && k < room; k++) {
        uint32_t at = buf + (uint32_t)k * SIZE_SCANINFO;
        zero(at, SIZE_SCANINFO);
        psp_write32(at, k + 1 < n && k + 1 < room ? at + SIZE_SCANINFO : 0);
        psp_write32(at + 4, 1);                        /* channel: 1 ("automatic" must not be 0) */
        write_bytes(at + 8, s[k].group, MHN_GROUP_LEN);
        write_mac(at + 16, s[k].bssid);
        psp_write32(at + 24, 0);                       /* mode: normal */
    }
    psp_write32(size_at, (uint32_t)(k * SIZE_SCANINFO));
    psp_ret(0);
}

static void hle_CtlGetState(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (!psp_arg(0)) { psp_ret(CTL_INVALID_ARG); return; }
    psp_write32(psp_arg(0), (uint32_t)g_state);
    psp_ret(0);
}

static void write_peer(uint32_t at, uint32_t next, const mhn_peer *p, uint64_t now) {
    zero(at, SIZE_PEERINFO);
    psp_write32(at, next);
    write_bytes(at + 4, p->nick, MHN_NICK_LEN);
    psp_write8(at + 4 + MHN_NICK_LEN - 1, 0);
    write_mac(at + 132, p->mac);
    psp_write32(at + 140, 0x400);
    psp_write32(at + 144, (uint32_t)now);
    psp_write32(at + 148, (uint32_t)(now >> 32));
}

static int group_peers(mhn_peer *out) {
    return g_state == ST_CONNECTED && !g_local_group ? mhn_ctl_peers(out, MHN_MAX_MEMBERS) : 0;
}

/* (int *size, SceNetAdhocctlPeerInfo *buf) */
static void hle_CtlGetPeerList(void) {
    uint32_t size_at = psp_arg(0), buf = psp_arg(1);
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (!size_at) { psp_ret(CTL_INVALID_ARG); return; }
    mhn_peer p[MHN_MAX_MEMBERS];
    int n = group_peers(p);
    if (!buf) { psp_write32(size_at, (uint32_t)(n * SIZE_PEERINFO)); psp_ret(0); return; }
    int room = (int)psp_read32(size_at) / SIZE_PEERINFO, k = 0;
    uint64_t now = guest_time_us();
    for (; k < n && k < room; k++) {
        uint32_t at = buf + (uint32_t)k * SIZE_PEERINFO;
        write_peer(at, k + 1 < n && k + 1 < room ? at + SIZE_PEERINFO : 0, &p[k], now);
    }
    psp_write32(size_at, (uint32_t)(k * SIZE_PEERINFO));
    psp_ret(0);
}

static int find_peer(const uint8_t mac[6], mhn_peer *out) {
    mhn_peer p[MHN_MAX_MEMBERS];
    int n = group_peers(p);
    for (int i = 0; i < n; i++) if (memcmp(p[i].mac, mac, 6) == 0) { *out = p[i]; return 1; }
    return 0;
}

/* (u8 *mac, int size, SceNetAdhocctlPeerInfo *info) */
static void hle_CtlGetPeerInfo(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    uint32_t mac_at = psp_arg(0), info = psp_arg(2);
    if (!mac_at || !info) { psp_ret(CTL_INVALID_ARG); return; }
    uint8_t mac[6], me[6];
    read_mac(mac_at, mac);
    mhfu_lobby_mac(me);
    mhn_peer p;
    if (memcmp(mac, me, 6) == 0) {
        memcpy(p.mac, me, 6);
        snprintf(p.nick, sizeof p.nick, "%s", mhfu_lobby_nick());
    } else if (!find_peer(mac, &p)) { psp_ret(ADHOC_NO_ENTRY); return; }
    write_peer(info, 0, &p, guest_time_us());
    psp_ret(0);
}

/* (u8 *mac, char *nickname[128]) */
static void hle_CtlGetNameByAddr(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    uint32_t mac_at = psp_arg(0), name_at = psp_arg(1);
    if (!mac_at || !name_at) { psp_ret(CTL_INVALID_ARG); return; }
    uint8_t mac[6], me[6];
    read_mac(mac_at, mac);
    mhfu_lobby_mac(me);
    char nick[MHN_NICK_LEN];
    mhn_peer p;
    if (memcmp(mac, me, 6) == 0) snprintf(nick, sizeof nick, "%s", mhfu_lobby_nick());
    else if (find_peer(mac, &p)) memcpy(nick, p.nick, sizeof nick);
    else { psp_ret(ADHOC_NO_ENTRY); return; }
    zero(name_at, MHN_NICK_LEN);
    write_bytes(name_at, nick, (uint32_t)strlen(nick));
    psp_ret(0);
}

/* (char *nickname, int *size, SceNetAdhocctlPeerInfo *buf) */
static void hle_CtlGetAddrByName(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    uint32_t name_at = psp_arg(0), size_at = psp_arg(1), buf = psp_arg(2);
    if (!name_at || !size_at) { psp_ret(CTL_INVALID_ARG); return; }
    char want[MHN_NICK_LEN];
    psp_str(name_at, want, sizeof want);
    mhn_peer p[MHN_MAX_MEMBERS], hit[MHN_MAX_MEMBERS];
    int n = group_peers(p), h = 0;
    for (int i = 0; i < n; i++) if (strncmp(p[i].nick, want, MHN_NICK_LEN) == 0) hit[h++] = p[i];
    if (!buf) { psp_write32(size_at, (uint32_t)(h * SIZE_PEERINFO)); psp_ret(0); return; }
    int room = (int)psp_read32(size_at) / SIZE_PEERINFO, k = 0;
    uint64_t now = guest_time_us();
    for (; k < h && k < room; k++) {
        uint32_t at = buf + (uint32_t)k * SIZE_PEERINFO;
        write_peer(at, k + 1 < h && k + 1 < room ? at + SIZE_PEERINFO : 0, &hit[k], now);
    }
    psp_write32(size_at, (uint32_t)(k * SIZE_PEERINFO));
    psp_ret(0);
}

/* (SceNetAdhocctlParameter *): channel, group_name[8], nickname[128], bssid[6]+pad */
static void hle_CtlGetParameter(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    uint32_t at = psp_arg(0);
    if (!at) { psp_ret(CTL_INVALID_ARG); return; }
    zero(at, 4 + MHN_GROUP_LEN + MHN_NICK_LEN + 8);
    psp_write32(at, 1);
    uint8_t bssid[6];
    mhfu_lobby_mac(bssid);
    if (g_state == ST_CONNECTED) {
        write_bytes(at + 4, g_group, MHN_GROUP_LEN);
        char group[MHN_GROUP_LEN];
        if (!g_local_group) mhn_ctl_group(group, bssid);
    }
    const char *nick = mhfu_lobby_nick();
    write_bytes(at + 12, nick, (uint32_t)strlen(nick));
    write_mac(at + 12 + MHN_NICK_LEN, bssid);
    psp_ret(0);
}

static void hle_CtlGetAdhocId(void) {
    if (!g_ctl_inited) { psp_ret(CTL_NOT_INITIALIZED); return; }
    if (psp_arg(0)) write_bytes(psp_arg(0), g_product, sizeof g_product);
    psp_ret(0);
}

/* ---- sceUtilityNetconf ----
 *
 * The system's network dialog. MHFU opens it to enter the Online Gathering
 * Hall: in its ad hoc actions it creates or joins the group named in
 * adhocparam, and finishes once connected (PPSSPP's PSPNetconfDialog). Here
 * that is the same group connect as sceNetAdhocctlCreate, through the room.
 * SceUtilityNetconfParam: common (48), netAction@48, adhocparam@52 ->
 * { char name[8]; u32 timeout; }. */

enum { NC_NONE, NC_INIT, NC_RUNNING, NC_FINISHED, NC_SHUTDOWN };
enum { NC_CONNECT_ADHOC = 2, NC_CREATE_ADHOC = 4, NC_JOIN_ADHOC = 5 };
#define UTILITY_INVALID_STATUS 0x80110001u
#define NC_RESULT_FAILED       1u   /* common.result for an action not done here */

static int      g_nc_status;
static uint32_t g_nc_param;
static int      g_nc_adhoc;         /* this dialog is joining a group */

static void hle_NetconfInitStart(void) {
    if (g_nc_status != NC_NONE) { psp_ret(UTILITY_INVALID_STATUS); return; }
    uint32_t p = psp_arg(0);
    if (!p) { psp_ret(CTL_INVALID_ARG); return; }
    const int action = (int)psp_read32(p + 48);
    const uint32_t adhoc = psp_read32(p + 52);
    char group[MHN_GROUP_LEN + 1];
    memset(group, 0, sizeof group);
    if (adhoc) for (int i = 0; i < MHN_GROUP_LEN; i++) group[i] = (char)psp_read8(adhoc + (uint32_t)i);
    g_nc_param = p;
    g_nc_status = NC_INIT;
    g_nc_adhoc = action == NC_CONNECT_ADHOC || action == NC_CREATE_ADHOC || action == NC_JOIN_ADHOC;
    printf("netconf: action %d, group \"%s\"%s\n", action, group, g_nc_adhoc ? "" : " (not an ad hoc action)");
    if (g_nc_adhoc) {
        if (!g_ctl_inited) { g_ctl_inited = 1; g_state = ST_DISCONNECTED; }
        if (g_state != ST_CONNECTED) connect_group(group);
    }
    psp_ret(0);
}

static void hle_NetconfGetStatus(void) {
    const int s = g_nc_status;
    if (s == NC_INIT) g_nc_status = NC_RUNNING;
    else if (s == NC_RUNNING && (!g_nc_adhoc || g_state == ST_CONNECTED)) {
        psp_write32(g_nc_param + 28, g_nc_adhoc ? 0 : NC_RESULT_FAILED);   /* common.result */
        g_nc_status = NC_FINISHED;
    } else if (s == NC_SHUTDOWN) g_nc_status = NC_NONE;
    psp_ret((uint32_t)s);
}

static void hle_NetconfUpdate(void) { psp_ret(0); }

static void hle_NetconfShutdownStart(void) {
    if (g_nc_status != NC_FINISHED && g_nc_status != NC_RUNNING) { psp_ret(UTILITY_INVALID_STATUS); return; }
    g_nc_status = NC_SHUTDOWN;
    psp_ret(0);
}

/* ---- handlers ---- */

static void call_handlers(int event, int error) {
    for (int i = 0; i < MAX_HANDLERS; i++) {
        uint32_t fn = g_handlers[i].fn;
        if (!fn) continue;
        if (!psp_lookup(fn)) {
            fprintf(stderr, "adhoc: handler 0x%08X is not a recompiled function; removed\n", fn);
            g_handlers[i].fn = 0;
            continue;
        }
        psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_SP] = (psp_cpu.r[PSP_REG_SP] - 64) & ~15u;
        psp_cpu.r[PSP_REG_A0] = (uint32_t)event;
        psp_cpu.r[PSP_REG_A1] = (uint32_t)error;
        psp_cpu.r[PSP_REG_A2] = g_handlers[i].arg;
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_call_from_host(fn);
        psp_cpu = saved;
    }
}

static void deliver(int event, int error) {
    switch (event) {
    case EV_CONNECT:    g_state = ST_CONNECTED; break;
    case EV_DISCONNECT: g_state = ST_DISCONNECTED; break;
    case EV_SCAN:       if (g_state == ST_SCANNING) g_state = ST_DISCONNECTED; break;
    default: break;
    }
    printf("adhoc: event %d (error 0x%08X), state %d\n", event, (uint32_t)error, g_state);
    call_handlers(event, error);
}

void mhfu_adhoc_tick(void) {
    if (!g_ctl_inited) return;
    static int busy;
    if (busy) return;
    busy = 1;
    while (g_nlocal_ev) {
        int e = g_local_ev[0][0], r = g_local_ev[0][1];
        memmove(g_local_ev[0], g_local_ev[1], (size_t)(g_nlocal_ev - 1) * sizeof g_local_ev[0]);
        g_nlocal_ev--;
        deliver(e, r);
    }
    int e, r;
    while (mhn_ctl_next_event(&e, &r)) deliver(e, r);
    /* The room went away under a connected group: tell the game. */
    if (g_state == ST_CONNECTED && !g_local_group && !online()) deliver(EV_DISCONNECT, 0);
    busy = 0;
}

void mhfu_adhoc_register(void) {
    psp_hle_register(0x39AF39A6, "sceNet", "sceNetInit", hle_NetInit);
    psp_hle_register(0x281928A9, "sceNet", "sceNetTerm", hle_NetTerm);
    psp_hle_register(0x0BF0A3AE, "sceNet", "sceNetGetLocalEtherAddr", hle_NetGetLocalEtherAddr);
    psp_hle_register(0xD7763699, "sceWlanDrv", "sceWlanGetSwitchState", hle_WlanGetSwitchState);
    psp_hle_register(0x0C622081, "sceWlanDrv", "sceWlanGetEtherAddr", hle_WlanGetEtherAddr);
    psp_hle_register(0xC69BEBCE, "sceOpenPSID", "sceOpenPSIDGetOpenPSID", hle_OpenPSIDGetOpenPSID);

    psp_hle_register(0xE1D621D7, "sceNetAdhoc", "sceNetAdhocInit", hle_AdhocInit);
    psp_hle_register(0xA62C6F57, "sceNetAdhoc", "sceNetAdhocTerm", hle_AdhocTerm);
    psp_hle_register(0x6F92741B, "sceNetAdhoc", "sceNetAdhocPdpCreate", hle_PdpCreate);
    psp_hle_register(0xABED3790, "sceNetAdhoc", "sceNetAdhocPdpSend", hle_PdpSend);
    psp_hle_register(0xDFE53E03, "sceNetAdhoc", "sceNetAdhocPdpRecv", hle_PdpRecv);
    psp_hle_register(0x7F27BB5E, "sceNetAdhoc", "sceNetAdhocPdpDelete", hle_PdpDelete);
    psp_hle_register(0xC7C1FC57, "sceNetAdhoc", "sceNetAdhocGetPdpStat", hle_GetPdpStat);
    psp_hle_register(0x877F6D66, "sceNetAdhoc", "sceNetAdhocPtpOpen", hle_PtpOpen);
    psp_hle_register(0xFC6FC07B, "sceNetAdhoc", "sceNetAdhocPtpConnect", hle_PtpConnect);
    psp_hle_register(0xE08BDAC1, "sceNetAdhoc", "sceNetAdhocPtpListen", hle_PtpListen);
    psp_hle_register(0x9DF81198, "sceNetAdhoc", "sceNetAdhocPtpAccept", hle_PtpAccept);
    psp_hle_register(0x4DA4C788, "sceNetAdhoc", "sceNetAdhocPtpSend", hle_PtpSend);
    psp_hle_register(0x8BEA2B3E, "sceNetAdhoc", "sceNetAdhocPtpRecv", hle_PtpRecv);
    psp_hle_register(0x157E6225, "sceNetAdhoc", "sceNetAdhocPtpClose", hle_PtpClose);
    psp_hle_register(0x9AC2EEAC, "sceNetAdhoc", "sceNetAdhocPtpFlush", hle_PtpFlush);
    psp_hle_register(0xB9685118, "sceNetAdhoc", "sceNetAdhocGetPtpStat", hle_GetPtpStat);
    psp_hle_register(0x7A662D6B, "sceNetAdhoc", "sceNetAdhocPollSocket", hle_PollSocket);
    psp_hle_register(0x73BFD52D, "sceNetAdhoc", "sceNetAdhocSetSocketAlert", hle_SetSocketAlert);
    psp_hle_register(0x4D2CE199, "sceNetAdhoc", "sceNetAdhocGetSocketAlert", hle_GetSocketAlert);

    psp_hle_register(0x4DB1E739, "sceUtility", "sceUtilityNetconfInitStart", hle_NetconfInitStart);
    psp_hle_register(0x6332AA39, "sceUtility", "sceUtilityNetconfGetStatus", hle_NetconfGetStatus);
    psp_hle_register(0x91E70E35, "sceUtility", "sceUtilityNetconfUpdate", hle_NetconfUpdate);
    psp_hle_register(0xF88155F6, "sceUtility", "sceUtilityNetconfShutdownStart", hle_NetconfShutdownStart);
    g_nc_status = NC_NONE;

    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit", hle_CtlInit);
    psp_hle_register(0x9D689E13, "sceNetAdhocctl", "sceNetAdhocctlTerm", hle_CtlTerm);
    psp_hle_register(0x20B317A0, "sceNetAdhocctl", "sceNetAdhocctlAddHandler", hle_CtlAddHandler);
    psp_hle_register(0x6402490B, "sceNetAdhocctl", "sceNetAdhocctlDelHandler", hle_CtlDelHandler);
    psp_hle_register(0xEC0635C1, "sceNetAdhocctl", "sceNetAdhocctlCreate", hle_CtlCreate);
    psp_hle_register(0x0AD043ED, "sceNetAdhocctl", "sceNetAdhocctlConnect", hle_CtlConnect);
    psp_hle_register(0x5E7F79C9, "sceNetAdhocctl", "sceNetAdhocctlJoin", hle_CtlJoin);
    psp_hle_register(0x34401D65, "sceNetAdhocctl", "sceNetAdhocctlDisconnect", hle_CtlDisconnect);
    psp_hle_register(0x08FFF7A0, "sceNetAdhocctl", "sceNetAdhocctlScan", hle_CtlScan);
    psp_hle_register(0x81AEE1BE, "sceNetAdhocctl", "sceNetAdhocctlGetScanInfo", hle_CtlGetScanInfo);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState", hle_CtlGetState);
    psp_hle_register(0xE162CB14, "sceNetAdhocctl", "sceNetAdhocctlGetPeerList", hle_CtlGetPeerList);
    psp_hle_register(0x8DB83FDC, "sceNetAdhocctl", "sceNetAdhocctlGetPeerInfo", hle_CtlGetPeerInfo);
    psp_hle_register(0x8916C003, "sceNetAdhocctl", "sceNetAdhocctlGetNameByAddr", hle_CtlGetNameByAddr);
    psp_hle_register(0x99560ABE, "sceNetAdhocctl", "sceNetAdhocctlGetAddrByName", hle_CtlGetAddrByName);
    psp_hle_register(0xDED9D28E, "sceNetAdhocctl", "sceNetAdhocctlGetParameter", hle_CtlGetParameter);
    psp_hle_register(0x362CBE8F, "sceNetAdhocctl", "sceNetAdhocctlGetAdhocId", hle_CtlGetAdhocId);
}
