/* MHFU-Android -- sockets, threads and a clock for the network code, on
 * Windows (Winsock) and POSIX (Linux, Android). Nothing here knows about the
 * PSP; hub.c, client.c and the standalone hub all build on it. */
#ifndef MHFU_NET_SYS_H
#define MHFU_NET_SYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_sock;
#define NET_BAD_SOCK INVALID_SOCKET
#else
typedef int net_sock;
#define NET_BAD_SOCK (-1)
#endif

/* Winsock wants starting once; harmless to call more than once. */
int  net_sys_init(void);

/* A TCP listener on every interface. `port` 0 picks one; the chosen port is
 * written back. */
net_sock net_tcp_listen(uint16_t *port);
/* Connect to host:port (a name or a dotted address), giving up after
 * `timeout_ms`. */
net_sock net_tcp_connect(const char *host, uint16_t port, int timeout_ms);
net_sock net_tcp_accept(net_sock s, char *peer, size_t peer_cap);
/* A UDP socket bound to `port` on every interface, broadcast allowed. */
net_sock net_udp_bind(uint16_t port);
int  net_udp_broadcast(net_sock s, uint16_t port, const void *buf, int len);
int  net_udp_recvfrom(net_sock s, void *buf, int cap, uint32_t *ip, uint16_t *port);
int  net_udp_sendto(net_sock s, uint32_t ip, uint16_t port, const void *buf, int len);

int  net_send_all(net_sock s, const void *buf, size_t len);
/* A send that cannot go out in `ms` fails (and net_send_all with it). */
void net_set_send_timeout(net_sock s, int ms);
/* > 0 bytes read, 0 the peer closed, < 0 an error. */
int  net_recv(net_sock s, void *buf, size_t cap);
void net_close(net_sock s);
void net_set_nodelay(net_sock s);

/* Wait until any of `socks` (up to NET_POLL_MAX) is readable, closed or in
 * error, or `timeout_ms` passes. ready[i] is set for each that is. Returns how
 * many, 0 on timeout, -1 on error. NET_BAD_SOCK entries are skipped. */
#define NET_POLL_MAX 512
int  net_poll(const net_sock *socks, int n, uint8_t *ready, int timeout_ms);

/* Dotted IPv4 of `ip` (host byte order). */
void net_ip_str(uint32_t ip, char *out, size_t cap);

/* ---- threads ---- */
typedef struct net_mutex net_mutex;
net_mutex *net_mutex_new(void);
void net_mutex_free(net_mutex *m);
void net_lock(net_mutex *m);
void net_unlock(net_mutex *m);

typedef void (*net_thread_fn)(void *arg);
/* A detached thread; 0 if it started. */
int  net_thread_start(net_thread_fn fn, void *arg);
void net_sleep_ms(int ms);

/* Monotonic milliseconds. */
uint64_t net_now_ms(void);

/* `n` bytes from the system's random source (falls back to the clock). */
void net_random(void *out, size_t n);

#endif
