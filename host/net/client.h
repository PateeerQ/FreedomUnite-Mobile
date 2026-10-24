/* MHFU-Android -- one player's connection to a hub, and the ad hoc state it
 * carries: the game's groups, the peers in ours, PDP datagram queues and PTP
 * connections. adhoc.c turns the PSP's sceNetAdhoc* calls into these.
 *
 * Every function is safe to call from any thread. None of them block on the
 * network: blocking with a timeout is the caller's job, so that it can let
 * other guest threads run while it waits. */
#ifndef MHFU_NET_CLIENT_H
#define MHFU_NET_CLIENT_H

#include <stdint.h>

#include "proto.h"

/* ---- the room ---- */

/* Connect to a hub and enter a room: `create` asks a public relay for a new
 * room, otherwise `code` names the room to join. Blocks up to a few seconds.
 * Returns MHN_OK or an MHN_* status, or -1 if the hub could not be reached. */
int  mhn_client_start(const char *host, uint16_t port, int create, const char *code,
                      const uint8_t mac[6], const char *nick);
void mhn_client_stop(void);
/* 1 while in a room. A lost connection drops back to 0. */
int  mhn_client_online(void);
/* The room's code ("MH-4K7Q"), or "" when not in one. */
void mhn_client_code(char code[MHN_CODE_LEN]);
void mhn_client_mac(uint8_t mac[6]);

typedef struct {
    uint8_t mac[6];
    char nick[MHN_NICK_LEN];
    uint64_t last_ms;
} mhn_peer;

/* Everyone else in the room, for the lobby screen. */
int  mhn_client_members(mhn_peer *out, int max);

/* Ask the LAN who holds room `code`: broadcasts and waits up to `timeout_ms`
 * for a hub to answer. Writes its address; 0 if one did. */
int  mhn_lan_find(const char *code, char *host, int host_cap, uint16_t *port, int timeout_ms);

/* ---- adhocctl ---- */

enum { MHN_EV_CONNECT = 1, MHN_EV_DISCONNECT = 2, MHN_EV_SCAN = 3 };

void mhn_ctl_connect(const char group[MHN_GROUP_LEN]);
void mhn_ctl_disconnect(void);
void mhn_ctl_scan(void);
/* Events for the game's adhocctl handlers, oldest first; 0 when none. */
int  mhn_ctl_next_event(int *event, int *error);
/* 1 if in a group (and which), else 0. */
int  mhn_ctl_group(char group[MHN_GROUP_LEN], uint8_t bssid[6]);

typedef struct {
    char group[MHN_GROUP_LEN];
    uint8_t bssid[6];
} mhn_scan_entry;

int  mhn_ctl_scan_results(mhn_scan_entry *out, int max);
/* The others in my group. */
int  mhn_ctl_peers(mhn_peer *out, int max);

/* ---- sockets (one id space for PDP and PTP, 1..MHN_MAX_SOCKETS) ---- */

#define MHN_MAX_SOCKETS 255

enum { MHN_ERR_PORT_IN_USE = -1, MHN_ERR_NO_SOCKET = -2, MHN_ERR_BAD_ID = -3 };

int  mhn_pdp_create(uint16_t port);
int  mhn_pdp_send(int id, const uint8_t dmac[6], uint16_t dport, const void *data, int len);
/* 0: a datagram was copied (*len its size). 1: none waiting. 2: *len is too
 * small; it now holds the size needed and nothing was taken. */
int  mhn_pdp_recv(int id, uint8_t smac[6], uint16_t *sport, void *buf, int *len);
/* Size of the next waiting datagram, 0 if none. */
int  mhn_pdp_next_size(int id);
uint16_t mhn_pdp_port(int id);
void mhn_pdp_delete(int id);

enum { MHN_PTP_CLOSED = 0, MHN_PTP_LISTEN = 1, MHN_PTP_SYN_SENT = 2, MHN_PTP_ESTABLISHED = 4 };

typedef struct {
    int id, state;
    uint8_t laddr[6], paddr[6];
    uint16_t lport, pport;
    uint32_t rcv_bytes;
    int peer_closed;
    int backlog;            /* a listener's connections waiting to be accepted */
} mhn_ptp_stat;

int  mhn_ptp_open(uint16_t sport, const uint8_t dmac[6], uint16_t dport);
/* Send the connect request (again, after a refusal). Returns the state. */
int  mhn_ptp_connect(int id);
int  mhn_ptp_listen(uint16_t sport, int backlog);
/* A new connected socket's id, 0 if nobody is waiting. */
int  mhn_ptp_accept(int listen_id, uint8_t mac[6], uint16_t *port);
/* Bytes sent, or -1 when not connected. */
int  mhn_ptp_send(int id, const void *data, int len);
/* 0: up to *len bytes copied (*len updated). 1: nothing yet. -1: the peer
 * closed and everything it sent has been read. */
int  mhn_ptp_recv(int id, void *buf, int *len);
int  mhn_ptp_stat_of(int id, mhn_ptp_stat *st);
int  mhn_ptp_list(mhn_ptp_stat *out, int max);
void mhn_ptp_close(int id);

/* Is `id` a PDP (1) or PTP (2) socket, or not open (0)? */
int  mhn_socket_kind(int id);

#endif
