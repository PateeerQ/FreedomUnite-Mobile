/* MHFU-Android -- see lobby.h. */
#include "lobby.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/client.h"
#include "net/hub.h"
#include "net/sys.h"

static char     g_cfg[700];
static char     g_nick[MHN_NICK_LEN] = "Hunter";
static uint8_t  g_mac[6];
static char     g_relay[256];
static uint16_t g_relay_port = MHN_PORT;
static mhn_hub *g_hub;           /* the in-app hub, while this app hosts a LAN room */
static int      g_on_relay;

static int parse_mac(const char *s, uint8_t mac[6]) {
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return 0;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void set_relay(const char *r) {
    snprintf(g_relay, sizeof g_relay, "%s", r ? r : "");
    g_relay_port = MHN_PORT;
    char *colon = strrchr(g_relay, ':');
    if (colon) { g_relay_port = (uint16_t)atoi(colon + 1); *colon = 0; }
}

static void save_cfg(void) {
    FILE *f = fopen(g_cfg, "w");
    if (!f) return;
    fprintf(f, "# MHFU-Android multiplayer settings\n");
    fprintf(f, "nick=%s\n", g_nick);
    fprintf(f, "mac=%02X:%02X:%02X:%02X:%02X:%02X\n", g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    fprintf(f, "# relay=host:port of a public mhfu_hub, for rooms over the internet\n");
    if (g_relay[0]) fprintf(f, "relay=%s:%u\n", g_relay, g_relay_port);
    fclose(f);
}

void mhfu_lobby_init(const char *dir, const char *nick, const char *mac, const char *relay) {
    snprintf(g_cfg, sizeof g_cfg, "%s/net.cfg", dir);
    int have_mac = 0;
    FILE *f = fopen(g_cfg, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            trim(line);
            if (!strncmp(line, "nick=", 5)) snprintf(g_nick, sizeof g_nick, "%s", line + 5);
            else if (!strncmp(line, "mac=", 4)) have_mac = parse_mac(line + 4, g_mac) == 0;
            else if (!strncmp(line, "relay=", 6)) set_relay(line + 6);
        }
        fclose(f);
    }
    if (!have_mac) {
        net_random(g_mac + 3, 3);
        g_mac[0] = 0x02;                 /* locally administered, unicast */
        g_mac[1] = 'M';
        g_mac[2] = 'H';
    }
    if (!f || !have_mac) save_cfg();
    if (nick && nick[0]) snprintf(g_nick, sizeof g_nick, "%s", nick);
    if (mac && mac[0]) parse_mac(mac, g_mac);
    if (relay && relay[0]) set_relay(relay);
}

static const char *status_text(int st) {
    switch (st) {
    case MHN_NO_ROOM:     return "no room with that code";
    case MHN_ROOM_FULL:   return "the room is full (16 players)";
    case MHN_BAD_VERSION: return "the server runs a different version";
    case MHN_MAC_IN_USE:  return "this device is already in that room";
    default:              return "could not reach the server";
    }
}

static void stop_hub(void) {
    if (!g_hub) return;
    mhn_hub_stop(g_hub);
    mhn_hub_free(g_hub);
    g_hub = NULL;
}

void mhfu_lobby_leave(void) {
    mhn_client_stop();
    stop_hub();
    g_on_relay = 0;
}

int mhfu_lobby_create(char *err, size_t cap) {
    mhfu_lobby_leave();
    if (g_relay[0]) {
        int st = mhn_client_start(g_relay, g_relay_port, 1, NULL, g_mac, g_nick);
        if (st == MHN_OK) { g_on_relay = 1; return 0; }
        snprintf(err, cap, "Relay %s: %s.", g_relay, status_text(st));
        return -1;
    }
    g_hub = mhn_hub_new(MHN_PORT, 1, 0);
    if (!g_hub) g_hub = mhn_hub_new(0, 1, 0);       /* the usual port is taken: any will do */
    char code[MHN_CODE_LEN];
    if (!g_hub || mhn_hub_make_room(g_hub, code) != 0 || mhn_hub_start_thread(g_hub) != 0) {
        stop_hub();
        snprintf(err, cap, "Could not start a server on this device.");
        return -1;
    }
    int st = mhn_client_start("127.0.0.1", mhn_hub_port(g_hub), 0, code, g_mac, g_nick);
    if (st != MHN_OK) {
        stop_hub();
        snprintf(err, cap, "The server on this device did not answer.");
        return -1;
    }
    return 0;
}

int mhfu_lobby_join(const char *code_in, char *err, size_t cap) {
    mhfu_lobby_leave();
    /* Accept "4k7q", "mh4k7q" or "MH-4K7Q". */
    char code[MHN_CODE_LEN], raw[16];
    size_t n = 0;
    for (const char *p = code_in; *p && n < sizeof raw - 1; p++)
        if (isalnum((unsigned char)*p)) raw[n++] = (char)toupper((unsigned char)*p);
    raw[n] = 0;
    const char *tail = (n == 6 && raw[0] == 'M' && raw[1] == 'H') ? raw + 2 : raw;
    if (strlen(tail) != 4) { snprintf(err, cap, "A room code looks like MH-4K7Q."); return -1; }
    snprintf(code, sizeof code, "MH-%s", tail);

    char host[64];
    uint16_t port;
    if (mhn_lan_find(code, host, sizeof host, &port, 1500) == 0) {
        int st = mhn_client_start(host, port, 0, code, g_mac, g_nick);
        if (st == MHN_OK) return 0;
        if (st != -1) { snprintf(err, cap, "%s: %s.", code, status_text(st)); return -1; }
    }
    if (g_relay[0]) {
        int st = mhn_client_start(g_relay, g_relay_port, 0, code, g_mac, g_nick);
        if (st == MHN_OK) { g_on_relay = 1; return 0; }
        snprintf(err, cap, "%s: %s.", code, status_text(st));
        return -1;
    }
    snprintf(err, cap, "No room %s on this network. (Over the internet, set relay= in net.cfg.)", code);
    return -1;
}

void mhfu_lobby_code(char code[8]) { mhn_client_code(code); }

void mhfu_lobby_status(char *out, size_t cap) {
    char code[MHN_CODE_LEN];
    mhn_client_code(code);
    if (!code[0]) { snprintf(out, cap, "Offline"); return; }
    mhn_peer m[MHN_MAX_MEMBERS];
    int n = mhn_client_members(m, MHN_MAX_MEMBERS) + 1;
    snprintf(out, cap, "Room %s (%s%s), %d player%s", code, g_on_relay ? "internet" : "LAN",
             g_hub ? ", hosting" : "", n, n == 1 ? "" : "s");
}

void mhfu_lobby_mac(uint8_t mac[6]) { memcpy(mac, g_mac, 6); }
const char *mhfu_lobby_nick(void) { return g_nick; }
