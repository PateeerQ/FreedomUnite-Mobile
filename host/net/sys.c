/* MHFU-Android -- see sys.h. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#else
#define FD_SETSIZE 1024     /* Winsock's select defaults to 64 sockets */
#endif
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

int net_sys_init(void) {
#ifdef _WIN32
    static int done;
    if (done) return 0;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
    done = 1;
#endif
    return 0;
}

void net_close(net_sock s) {
    if (s == NET_BAD_SOCK) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void net_set_nodelay(net_sock s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static void set_reuse(net_sock s) {
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
}

net_sock net_tcp_listen(uint16_t *port) {
    net_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == NET_BAD_SOCK) return s;
    set_reuse(s);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(*port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 16) != 0) {
        net_close(s);
        return NET_BAD_SOCK;
    }
    socklen_t len = sizeof a;
    if (getsockname(s, (struct sockaddr *)&a, &len) == 0) *port = ntohs(a.sin_port);
    return s;
}

static void set_blocking(net_sock s, int blocking) {
#ifdef _WIN32
    u_long nb = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

net_sock net_tcp_connect(const char *host, uint16_t port, int timeout_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%u", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return NET_BAD_SOCK;

    net_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == NET_BAD_SOCK) { freeaddrinfo(res); return s; }
    set_blocking(s, 0);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0) {
        /* In progress: wait for it to become writable, then ask how it went. */
#ifdef _WIN32
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(0, NULL, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { net_close(s); return NET_BAD_SOCK; }
#else
        struct pollfd p = { s, POLLOUT, 0 };
        if (poll(&p, 1, timeout_ms) <= 0) { net_close(s); return NET_BAD_SOCK; }
#endif
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err) { net_close(s); return NET_BAD_SOCK; }
    }
    set_blocking(s, 1);
    net_set_nodelay(s);
    return s;
}

net_sock net_tcp_accept(net_sock s, char *peer, size_t peer_cap) {
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    net_sock c = accept(s, (struct sockaddr *)&a, &len);
    if (c != NET_BAD_SOCK) {
        net_set_nodelay(c);
        if (peer) net_ip_str(ntohl(a.sin_addr.s_addr), peer, peer_cap);
    }
    return c;
}

net_sock net_udp_bind(uint16_t port) {
    net_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == NET_BAD_SOCK) return s;
    set_reuse(s);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { net_close(s); return NET_BAD_SOCK; }
    return s;
}

int net_udp_sendto(net_sock s, uint32_t ip, uint16_t port, const void *buf, int len) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    return (int)sendto(s, (const char *)buf, len, 0, (struct sockaddr *)&a, sizeof a);
}

int net_udp_broadcast(net_sock s, uint16_t port, const void *buf, int len) {
    return net_udp_sendto(s, 0xFFFFFFFFu, port, buf, len);
}

int net_udp_recvfrom(net_sock s, void *buf, int cap, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    int n = (int)recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)&a, &len);
    if (n >= 0) {
        if (ip) *ip = ntohl(a.sin_addr.s_addr);
        if (port) *port = ntohs(a.sin_port);
    }
    return n;
}

int net_send_all(net_sock s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        int n = (int)send(s, p, (int)(len > 65536 ? 65536 : len), 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int net_recv(net_sock s, void *buf, size_t cap) {
    return (int)recv(s, (char *)buf, (int)cap, 0);
}

int net_poll(const net_sock *socks, int n, uint8_t *ready, int timeout_ms) {
    if (n > NET_POLL_MAX) n = NET_POLL_MAX;
    memset(ready, 0, (size_t)n);
#ifdef _WIN32
    fd_set r;
    FD_ZERO(&r);
    int any = 0;
    for (int i = 0; i < n; i++) if (socks[i] != NET_BAD_SOCK) { FD_SET(socks[i], &r); any = 1; }
    if (!any) { Sleep((DWORD)timeout_ms); return 0; }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int k = select(0, &r, NULL, NULL, &tv);
    if (k <= 0) return k;
    int cnt = 0;
    for (int i = 0; i < n; i++)
        if (socks[i] != NET_BAD_SOCK && FD_ISSET(socks[i], &r)) { ready[i] = 1; cnt++; }
    return cnt;
#else
    struct pollfd p[NET_POLL_MAX];
    for (int i = 0; i < n; i++) { p[i].fd = socks[i]; p[i].events = POLLIN; p[i].revents = 0; }
    int k = poll(p, (nfds_t)n, timeout_ms);
    if (k <= 0) return k;
    int cnt = 0;
    for (int i = 0; i < n; i++)
        if (socks[i] != NET_BAD_SOCK && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) { ready[i] = 1; cnt++; }
    return cnt;
#endif
}

void net_ip_str(uint32_t ip, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

/* ---- threads ---- */

struct net_mutex {
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t m;
#endif
};

net_mutex *net_mutex_new(void) {
    net_mutex *m = (net_mutex *)calloc(1, sizeof *m);
    if (!m) return NULL;
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->m, NULL);
#endif
    return m;
}

void net_mutex_free(net_mutex *m) {
    if (!m) return;
#ifdef _WIN32
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->m);
#endif
    free(m);
}

void net_lock(net_mutex *m) {
#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->m);
#endif
}

void net_unlock(net_mutex *m) {
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->m);
#endif
}

typedef struct { net_thread_fn fn; void *arg; } thread_start;

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    thread_start ts = *(thread_start *)p;
    free(p);
    ts.fn(ts.arg);
    return 0;
}
#else
static void *thread_main(void *p) {
    thread_start ts = *(thread_start *)p;
    free(p);
    ts.fn(ts.arg);
    return NULL;
}
#endif

int net_thread_start(net_thread_fn fn, void *arg) {
    thread_start *ts = (thread_start *)malloc(sizeof *ts);
    if (!ts) return -1;
    ts->fn = fn;
    ts->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, thread_main, ts, 0, NULL);
    if (!h) { free(ts); return -1; }
    CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, ts) != 0) { free(ts); return -1; }
    pthread_detach(t);
#endif
    return 0;
}

void net_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
#endif
}

uint64_t net_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

#ifdef _WIN32
#define SystemFunction036 NTAPI SystemFunction036
#include <ntsecapi.h>
#undef SystemFunction036
#endif

void net_random(void *out, size_t n) {
#ifdef _WIN32
    if (RtlGenRandom(out, (ULONG)n)) return;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
#endif
    uint64_t x = net_now_ms() ^ (uint64_t)(uintptr_t)out ^ ((uint64_t)time(NULL) << 20);
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        ((uint8_t *)out)[i] = (uint8_t)x;
    }
}
