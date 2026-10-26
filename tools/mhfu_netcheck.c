/* MHFU-Android -- an end-to-end check of the room network, without the game.
 *
 *     mhfu_netcheck host            create a room (in-app hub), wait for a guest
 *     mhfu_netcheck join CODE       find the room on the LAN and talk to the host
 *     mhfu_netcheck relay HOST      create a room on a standalone hub, then
 *     mhfu_netcheck join CODE HOST  join it there
 *
 * The host joins ad hoc group "MHFUTEST", listens for PTP on port 200 and
 * echoes PDP on port 100. The guest scans, joins the group, sends a broadcast
 * and a unicast datagram, opens a PTP connection and checks the echo. Both
 * print PASS or FAIL. With --pass 1234 the room has that passcode: the guest
 * checks the list shows it locked and that a wrong passcode is refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/client.h"
#include "net/hub.h"
#include "net/sys.h"

static const char TEST_GROUP[MHN_GROUP_LEN] = "MHFUTEST";
static const char *g_pass = "";      /* --pass 1234: the room's passcode */

static int wait_event(int want, int ms) {
    uint64_t until = net_now_ms() + (uint64_t)ms;
    while (net_now_ms() < until) {
        int ev, err;
        while (mhn_ctl_next_event(&ev, &err)) if (ev == want) return 0;
        net_sleep_ms(10);
    }
    return -1;
}

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); return 1; } printf("ok: %s\n", what); } while (0)

static int host(const char *relay) {
    char code[MHN_CODE_LEN];
    mhn_hub *hub = NULL;
    uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    if (relay) {
        mhn_client_set_fps(60);                       /* the host could play at 60 ... */
        CHECK(mhn_client_start(relay, MHN_PORT, 1, NULL, mac, "Host", g_pass) == MHN_OK, "room created on the relay");
    } else {
        hub = mhn_hub_new(0, 1, 0);
        CHECK(hub && mhn_hub_make_room(hub, code, g_pass, "Host") == 0 && mhn_hub_start_thread(hub) == 0,
              "in-app hub up");
        mhn_client_set_fps(60);
        CHECK(mhn_client_start("127.0.0.1", mhn_hub_port(hub), 0, code, mac, "Host", g_pass) == MHN_OK,
              "host in its own room");
    }
    mhn_client_code(code);
    printf("ROOM %s\n", code);
    fflush(stdout);

    mhn_ctl_connect(TEST_GROUP);
    CHECK(wait_event(MHN_EV_CONNECT, 3000) == 0, "group created");
    int pdp = mhn_pdp_create(100);
    int lis = mhn_ptp_listen(200, 4);
    CHECK(pdp > 0 && lis > 0, "sockets open");

    int echoed = 0, accepted = 0, conn = 0, streamed = 0, chats = 0, slowed = 0;
    uint64_t until = net_now_ms() + 30000, quiet_since = 0;
    /* Until the guest's chat lines have stopped coming for 2 s (a hub drops
     * all but MHN_CHAT_BURST of them). */
    while (net_now_ms() < until && !(echoed >= 2 && streamed >= 100000 && chats && net_now_ms() - quiet_since > 2000)) {
        uint8_t from[6], buf[2048];
        uint16_t port;
        int len = sizeof buf;
        char nick[MHN_NICK_LEN], line[MHN_CHAT_MAX + 1];
        int joined;
        while (mhn_chat_next(nick, sizeof nick, line, sizeof line, &joined)) {   /* every chat line, answered */
            if (joined == 2 && !strcmp(line, "30") && !strcmp(nick, "Guest")) slowed = 1;
            if (joined) continue;                                             /* the guest coming in */
            char reply[MHN_CHAT_MAX + 1];
            snprintf(reply, sizeof reply, "re: %s", line);
            mhn_chat_send(reply);
            chats++;
            quiet_since = net_now_ms();
        }
        if (mhn_pdp_recv(pdp, from, &port, buf, &len) == 0) {
            mhn_pdp_send(pdp, from, port, buf, len);
            echoed++;
        }
        if (!conn) {
            uint8_t pm[6];
            uint16_t pp;
            conn = mhn_ptp_accept(lis, pm, &pp);
            if (conn < 0) conn = 0;
        }
        if (conn) {
            len = sizeof buf;
            if (mhn_ptp_recv(conn, buf, &len) == 0) { mhn_ptp_send(conn, buf, len); accepted = 1; streamed += len; }
        }
        net_sleep_ms(5);
    }
    CHECK(echoed >= 2, "echoed the guest's datagrams");
    CHECK(accepted, "accepted and echoed a PTP connection");
    CHECK(chats == MHN_CHAT_BURST, "the guest's chat lines came, all but the ones over the limit");
    CHECK(slowed, "the room went to 30 fps with the guest, its slowest player");
    net_sleep_ms(1500);   /* let the guest read the echo before the room closes */
    mhn_ptp_close(conn);
    mhn_client_stop();
    if (hub) { mhn_hub_stop(hub); mhn_hub_free(hub); }
    printf("PASS\n");
    return 0;
}

static int join(const char *code, const char *relay) {
    char host[64];
    uint16_t port = MHN_PORT;
    if (relay) snprintf(host, sizeof host, "%s", relay);
    else CHECK(mhn_lan_find(code, host, sizeof host, &port, 3000) == 0, "room found on the LAN");
    uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

    /* The room is listed, with its passcode lock. */
    mhn_room_info rooms[16];
    const int listed = relay ? mhn_hub_list(host, port, rooms, 16, 3000) : mhn_lan_list(rooms, 16, 1000);
    int seen = -1;
    for (int i = 0; i < listed; i++) if (!strcmp(rooms[i].code, code)) seen = i;
    CHECK(seen >= 0 && rooms[seen].players == 1 && rooms[seen].locked == (g_pass[0] != 0),
          "the room is in the list, one player, locked as it should be");
    if (g_pass[0])
        CHECK(mhn_client_start(host, port, 0, code, mac, "Guest", "0000") == MHN_BAD_PASSCODE,
              "a wrong passcode is refused");

    mhn_client_set_fps(30);                           /* ... the guest only at 30 */
    CHECK(mhn_client_start(host, port, 0, code, mac, "Guest", g_pass) == MHN_OK, "joined the room");
    for (int i = 0; i < 100 && mhn_client_room_fps() != 30; i++) net_sleep_ms(10);
    CHECK(mhn_client_room_fps() == 30, "the room plays at 30 fps, the slowest player's");

    mhn_ctl_scan();
    CHECK(wait_event(MHN_EV_SCAN, 3000) == 0, "scan finished");
    mhn_scan_entry s[16];
    int n = mhn_ctl_scan_results(s, 16);
    CHECK(n == 1 && memcmp(s[0].group, TEST_GROUP, MHN_GROUP_LEN) == 0, "scan sees the host's group");
    uint8_t host_mac[6];
    memcpy(host_mac, s[0].bssid, 6);

    mhn_ctl_connect(TEST_GROUP);
    CHECK(wait_event(MHN_EV_CONNECT, 3000) == 0, "joined the group");
    net_sleep_ms(200);
    mhn_peer peers[16];
    CHECK(mhn_ctl_peers(peers, 16) == 1 && strcmp(peers[0].nick, "Host") == 0, "peer list has the host");

    int pdp = mhn_pdp_create(0);
    static const uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    mhn_pdp_send(pdp, bc, 100, "broadcast", 9);
    mhn_pdp_send(pdp, host_mac, 100, "unicast", 7);
    int got = 0;
    uint64_t until = net_now_ms() + 3000;
    while (got < 2 && net_now_ms() < until) {
        char buf[64];
        int len = sizeof buf;
        if (mhn_pdp_recv(pdp, NULL, NULL, buf, &len) == 0) got++;
        else net_sleep_ms(5);
    }
    CHECK(got == 2, "both datagrams echoed");

    int c = mhn_ptp_open(0, host_mac, 200);
    mhn_ptp_connect(c);
    until = net_now_ms() + 3000;
    mhn_ptp_stat st;
    do { mhn_ptp_stat_of(c, &st); net_sleep_ms(5); } while (st.state != MHN_PTP_ESTABLISHED && net_now_ms() < until);
    CHECK(st.state == MHN_PTP_ESTABLISHED, "PTP connected");
    static char big[100000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)(i * 7);
    mhn_ptp_send(c, big, sizeof big);
    static char back[100000];
    int have = 0;
    until = net_now_ms() + 5000;
    while (have < (int)sizeof back && net_now_ms() < until) {
        int len = (int)sizeof back - have;
        if (mhn_ptp_recv(c, back + have, &len) == 0) have += len;
        else net_sleep_ms(2);
    }
    CHECK(have == (int)sizeof big && memcmp(big, back, sizeof big) == 0, "100 KB PTP echo intact");

    int r = mhn_ptp_open(0, host_mac, 999);
    mhn_ptp_connect(r);
    net_sleep_ms(500);
    mhn_ptp_stat_of(r, &st);
    CHECK(st.state == MHN_PTP_CLOSED, "connect to a closed port refused");

    /* Chat: MHN_CHAT_BURST + 2 lines at once; the host answers every one it
     * gets, so the hub's limit shows as exactly MHN_CHAT_BURST answers. */
    for (int i = 0; i < MHN_CHAT_BURST + 2; i++) {
        char line[32];
        snprintf(line, sizeof line, "hello %d", i);
        mhn_chat_send(line);
    }
    int answers = 0, from_host = 1;
    until = net_now_ms() + 4000;
    while (net_now_ms() < until) {
        char nick[MHN_NICK_LEN], line[MHN_CHAT_MAX + 1];
        int joined;
        if (mhn_chat_next(nick, sizeof nick, line, sizeof line, &joined)) {
            if (joined) continue;
            answers++;
            from_host &= !strcmp(nick, "Host") && !strncmp(line, "re: hello ", 10);
        } else net_sleep_ms(10);
    }
    CHECK(answers == MHN_CHAT_BURST && from_host, "chat lines answered by the host, the extra ones held back");
    mhn_client_stop();
    printf("PASS\n");
    return 0;
}

/* Into a room as "Tester", one chat line, out: to see the chat in a game. */
static int say(const char *code, const char *relay, const char *text) {
    char host[64];
    uint16_t port = MHN_PORT;
    if (relay) {
        snprintf(host, sizeof host, "%s", relay);
        char *colon = strrchr(host, ':');                 /* HOST:PORT */
        if (colon) { port = (uint16_t)atoi(colon + 1); *colon = 0; }
    } else CHECK(mhn_lan_find(code, host, sizeof host, &port, 3000) == 0, "room found on the LAN");
    const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
    CHECK(mhn_client_start(host, port, 0, code, mac, "Tester", g_pass) == MHN_OK, "joined the room");
    CHECK(mhn_chat_send(text) == 0, "chat line sent");
    net_sleep_ms(1000);
    mhn_client_stop();
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3 && !strcmp(argv[argc - 2], "--pass")) { g_pass = argv[argc - 1]; argc -= 2; }
    if (argc >= 2 && !strcmp(argv[1], "host")) return host(NULL);
    if (argc >= 3 && !strcmp(argv[1], "relay")) return host(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "join")) return join(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 4 && !strcmp(argv[1], "say")) return say(argv[2], argc >= 5 ? argv[3] : NULL, argv[argc - 1]);
    fprintf(stderr, "usage: mhfu_netcheck host | relay HOST | join CODE [HOST] | say CODE [HOST] TEXT   [--pass 1234]\n");
    return 2;
}
