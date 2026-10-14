/*
 * mp_net.h -- the transport under runtime/mp_session.c.
 *
 * GKSession did three things for the iPhone game: it showed which devices
 * nearby had the multiplayer screen open, it connected two of them when one
 * player picked the other and the other agreed, and it carried their packets.
 * This is the same three things. Online play is the browser build's, and its
 * one backend is web/mp_webrtc.js: a lobby service (server/signal.js) for the
 * first two, a WebRTC data channel for the third. The native builds keep the
 * stubs in runtime/gamecode_stubs.c, the radio switched off.
 *
 * Every call is polled from the game's own frame (mp_session.c), so the
 * backend never calls into the game.
 */
#ifndef UMK3_MP_NET_H
#define UMK3_MP_NET_H

enum {
    MPNET_OFF,          /* not started */
    MPNET_LOBBY,        /* listed, looking at the other players */
    MPNET_CONNECTING,   /* an invitation is out, or the link is being set up */
    MPNET_CONNECTED,    /* the data channel is open */
    MPNET_CLOSED        /* the link (or the lobby) went away */
};

/* Into the lobby, as a player running build `build` -- two builds only see
 * each other when this matches, because the sprite list on the wire is this
 * port's memory layout. */
void mpnet_start(unsigned build);
void mpnet_stop(void);
/* Back into the lobby after a match or a dropped link, still started. */
void mpnet_relist(void);
int  mpnet_state(void);

int  mpnet_peer_count(void);
/* The name of lobby row `i`, NUL-terminated in `out` (`n` bytes); 0 when the
 * row is gone. */
int  mpnet_peer_name(int i, char *out, int n);
/* Ask row `i` for a match: 1 when the invitation went out. */
int  mpnet_invite(int i);
/* An invitation waiting for an answer: 1 and the inviter's name in `out`. */
int  mpnet_invitation(char *out, int n);
void mpnet_answer(int accept);

/* Once connected: whether this side runs the fight (the game's parent), and
 * the opponent's name. */
int  mpnet_is_host(void);
void mpnet_partner_name(char *out, int n);

/* One message, reliable and in order; 0 when there is no link. */
int  mpnet_send(const void *p, int len);
/* The next message: its length, 0 when none waits, -1 when it did not fit
 * (and was dropped). */
int  mpnet_recv(void *p, int cap);

#endif
