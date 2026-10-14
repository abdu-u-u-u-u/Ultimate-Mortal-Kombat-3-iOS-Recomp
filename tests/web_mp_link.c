/*
 * web_mp_link.c -- the browser transport (web/mp_webrtc.js) end to end, in
 * two real browser tabs, with no game.
 *
 *   emcc -O1 tests/web_mp_link.c -Iruntime --js-library web/mp_webrtc.js \
 *        -sASYNCIFY -o build-web/mp_link.html
 *
 * Open the page twice (the same ?room= in both, e.g. ?room=linktest). The
 * first tab to see a peer invites it; the other accepts. Once the data
 * channel is open each side sends 200 numbered messages of growing size (up
 * to 2 KB, larger than a sprite-list packet) and checks that the other's
 * arrive complete and in order. The last line printed is PASS or FAIL.
 */
#include <stdio.h>
#include <string.h>
#include <emscripten.h>

#include "mp_net.h"

#define N_MSG 200

int main(void)
{
    char name[64];
    unsigned char buf[4096];
    int sent = 0, got = 0, ok = 1, invited = 0, t;

    printf("lobby...\n");
    mpnet_start(0x4c494e4b);                  /* "LINK": its own lobby */

    for (t = 0; t < 60 * 60; t++) {           /* a minute to find a partner */
        int st = mpnet_state();
        if (st == MPNET_CONNECTED)
            break;
        if (st == MPNET_CLOSED) {
            printf("FAIL: link closed while connecting\n");
            return 1;
        }
        if (mpnet_invitation(name, sizeof name)) {
            printf("invited by %s: accepting\n", name);
            mpnet_answer(1);
        } else if (!invited && st == MPNET_LOBBY && mpnet_peer_count() > 0) {
            mpnet_peer_name(0, name, sizeof name);
            printf("inviting %s\n", name);
            invited = mpnet_invite(0);
        }
        emscripten_sleep(16);
    }
    if (mpnet_state() != MPNET_CONNECTED) {
        printf("FAIL: nobody to play with\n");
        return 1;
    }
    mpnet_partner_name(name, sizeof name);
    printf("connected to %s, %s\n", name, mpnet_is_host() ? "host" : "guest");

    for (t = 0; t < 60 * 30 && (sent < N_MSG || got < N_MSG); t++) {
        int n;

        if (sent < N_MSG) {                   /* a few a frame, like a tick */
            int i, len = 8 + sent * 10;
            for (i = 0; i < len; i++)
                buf[i] = (unsigned char)(sent + i);
            memcpy(buf, &sent, sizeof sent);
            if (mpnet_send(buf, len))
                sent++;
        }
        while ((n = mpnet_recv(buf, sizeof buf)) != 0) {
            int k, i, want;
            if (n < 0) { ok = 0; printf("FAIL: oversize message\n"); break; }
            memcpy(&k, buf, sizeof k);
            want = 8 + got * 10;
            if (k != got || n != want) {
                ok = 0;
                printf("FAIL: message %d arrived as %d (%d bytes, want %d)\n",
                       got, k, n, want);
            }
            for (i = sizeof k; i < n && ok; i++)
                if (buf[i] != (unsigned char)(k + i)) {
                    ok = 0;
                    printf("FAIL: message %d corrupt at byte %d\n", k, i);
                }
            got++;
        }
        if (mpnet_state() != MPNET_CONNECTED && got < N_MSG) {
            printf("FAIL: link dropped after %d sent, %d received\n", sent, got);
            return 1;
        }
        emscripten_sleep(16);
    }
    printf("%s: sent %d, received %d in order\n",
           ok && got == N_MSG ? "PASS" : "FAIL", sent, got);
    emscripten_sleep(2000);                   /* let the last ones drain */
    mpnet_stop();
    return 0;
}
