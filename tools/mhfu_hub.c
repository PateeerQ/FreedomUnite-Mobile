/* MHFU-Android -- the standalone room hub, for a server reachable from the
 * internet (a VPS, a home PC with one forwarded port, ...). Players never
 * forward anything: "Create Server" in the app asks this hub for a new room
 * and shows its code, "Join Server" enters the code. See host/net/proto.h.
 *
 *     mhfu_hub [port]        (default 27313; also answers LAN discovery)
 */
#include <stdio.h>
#include <stdlib.h>

#include "net/hub.h"
#include "net/proto.h"

int main(int argc, char **argv) {
    unsigned port = argc > 1 ? (unsigned)atoi(argv[1]) : MHN_PORT;
    mhn_hub *h = mhn_hub_new((uint16_t)port, 1, 1);
    if (!h) {
        fprintf(stderr, "mhfu_hub: cannot listen on TCP port %u\n", port);
        return 1;
    }
    printf("mhfu_hub: rooms on TCP port %u\n", mhn_hub_port(h));
    fflush(stdout);
    mhn_hub_run(h);
    mhn_hub_free(h);
    return 0;
}
