/*
 * mp_session.c -- the multiplayer layer the game calls, over runtime/mp_net.h.
 *
 * The iOS game's two-player mode lived in an Objective-C layer that is not in
 * decomp/ (limeMPSession, limeMPGameSpecific: ~126 functions over GameKit's
 * GKSession). The decompiled game calls 31 functions of it -- startMP,
 * isParent, sendCharacterPacket, syncGame, setNextSpritesAndEvents ... --
 * which runtime/gamecode_stubs.c answers "radio off" in the native builds.
 * This file is those 31 functions for the browser build (UMK3_HAVE_MP), with
 * the transport in web/mp_webrtc.js.
 *
 * ## Whose rules
 *
 * The wire format is this port's own: both ends run the same build (the
 * lobby only pairs equal builds, MP_WIRE), so nothing here has to match
 * what two iPhones sent each other. What each packet must DO on arrival is
 * not ours: it is read off the decompiled consumers -- the globals the front
 * end and the fight wait on that nothing in decomp/ writes, and five
 * decompiled functions nothing in decomp/ calls (switchToTask,
 * switchToFETask, processCharacterSelectedPacket, get/setTransferableFlags),
 * which are the hooks this layer called. Where a rule below is a port
 * decision rather than something the consumers show, it says so.
 *
 * ## Roles
 *
 * The lobby makes the player who accepted the invitation the host.
 * isParent() and isParentBasedOnSpeed() both answer "host" during a session
 * and 0 outside one: the iPhone elected the faster device to run the fight
 * (doFPSExchange); two browsers at a fixed 60 Hz have nothing to elect, and
 * the decompiled code is only consistent when the two agree (the session
 * parent is assumed to be player 1 in UpdateStats, CountFinisherStat and the
 * select screen's labels).
 *
 * The host runs the fight engine with both joystick words -- its own and the
 * last one the guest sent -- and sends the resulting sprite list, positions,
 * event queue and round-live flag every engine tick. The guest never steps
 * the engine; it draws what arrives (UpdateArcadeCode).
 *
 * ## When things happen
 *
 * Everything is polled: heartbeatUpdate(), which GameCodeMain calls every
 * frame, drains the link; setNextSpritesAndEvents() also drains it, so a
 * list that arrived this frame is drawn this frame. Sprite lists, joystick
 * words and the ready barrier are queued the moment they arrive; every other
 * packet is applied at the end of a frame, in arrival order, where the
 * front end's task stack and the fight's fade can be changed safely.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp_net.h"
#include "platform/platform.h"

/* Bumped whenever a packet below changes; the lobby pairs equal values only. */
#define MP_WIRE 0x554d0001u                     /* "UM", wire 1 */

/* ------------------------------------------------------------ the game ---- */

extern long  GameMode;                  /* 1 during a session */
extern long  GamePaused;                /* 0, 1 the pause menu, 2 the moves list */
extern long  otherPlayerPaused;
extern int   CurrentTask;               /* TaskFunctionList: 3 front end, 6 fight */
extern int   FE_CurrentTask;            /* 0x28 post-match, 0x2a select, 0x2b VS, 0x2c gone */
extern long  WaitForOpponent;
extern int   opponentCharacter;
extern long  opponentPressedPlay;
extern long  LevelSelect;
extern int   KodeSelector[10];
extern float KodeSelectorParticle[10];
extern float vsScreenTimer, VSWait;
extern long  RoundWins[2], WinsNeeded;
extern long  PauseMenuAreYouSure, RoundHasEnded, DontQuitAfterFade;
extern float *FE_FadeAddP;
extern long *requestedLevel;
extern long *startTime;                 /* read as a float by Task_MultiplayerSync */
extern void *G;                         /* GAMESTATE *: the fight engine's block */

extern long  SpritelistReceived;
extern long  currentMPSpriteList, currentMPJoystickData;
extern long  mpOpponentJoystickInput[4];/* two entries of { word, fps } */
extern long  opponentFPS2;
extern char  mpSpriteList[0x140];       /* two lists of ten 16-byte records */
extern char  mpEventQueue[0x1b0];       /* two of { P1 pos, P2 pos, 0x54 events } */
extern long *MKEventQueue;
extern float Player1Pos[3], Player2Pos[3];
extern char  dummyObjects[32];

void switchToTask(int task);
void switchToFETask(int task);
void processCharacterSelectedPacket(void);
int  getTransferableFlags(void);
void setTransferableFlags(long flags);
void PushFETask(int task);
void PopFETaskDeferred(void);
void PopFETaskDeferred2(void);
void resetCharacterSelection(void);
int  getRandomLevel(void);
void checkIfKode(void);
void preprocessPreloadKode(void);
void QuitAsWin(void);

void enableHeartbeat(long seconds);
void disableHeartbeat(void);

#define TASK_FE         3
#define TASK_FIGHT      6
#define TASK_SYNC       9
#define FE_POSTMATCH    0x28
#define FE_VERSUS       0x2b
#define FE_DISCONNECTED 0x2c

#define SPRITE_STRIDE   160             /* UpdateArcadeCode's MP_SPRITELIST_STRIDE */
#define EVENTQ_STRIDE   216             /* MP_EVENTQUEUE_STRIDE */
#define EVENT_BYTES     0x54            /* a count and ten 8-byte events */
#define MAX_NODES       (SPRITE_STRIDE / 16)
#define WHO_IN_FRONT    0x474           /* G + 0x474: mk3_who_in_front */

/* ------------------------------------------------------------- packets ---- */

enum {
    MPK_PING = 1,    /* keep-alive; any packet counts as one */
    MPK_READY,       /* syncGame: loaded and waiting                       */
    MPK_CHAR,        /* i32 character    -> opponentCharacter              */
    MPK_PLAY,        /*                  -> opponentPressedPlay            */
    MPK_FEMENU,      /* i32 item         -> push it, or pop one / two      */
    MPK_MENU,        /* i32 task (4)     -> switchToTask: into the fight   */
    MPK_GENERIC,     /* i32 a, b         -> the peer is on the post-match  */
    MPK_LEVEL,       /* i32 level        -> LevelSelect (guest)            */
    MPK_KODE,        /* i32 digit, value -> KodeSelector[digit]            */
    MPK_TIMER,       /* i32 timer, wait  -> the VS countdown (guest)       */
    MPK_PAUSE,       /* i32 state        -> otherPlayerPaused, GamePaused  */
    MPK_QUIT,        /* i32 natural      -> the match ended or was left    */
    MPK_JOY,         /* i32 word         -> the host's joy[1]              */
    MPK_SPRITES      /* the tick, host -> guest: see send_sprites()        */
};

/* The sprite list as it travels: everything the guest's UpdateArcadeCode
 * reads out of mpSpriteList / mpEventQueue, plus the two words of the
 * engine's block it needs and cannot compute without running the engine. */
typedef struct {
    int32_t n;                          /* records in the display list */
    int32_t flags;                      /* getTransferableFlags: G+0x44e */
    int32_t front;                      /* G+0x474, port addition (see below) */
    uint8_t pos[24];                    /* Player1Pos, Player2Pos */
    uint8_t events[EVENT_BYTES];
    uint8_t nodes[MAX_NODES][12];       /* each record past its link word */
} SPRITES;

#define SPRITE_QUEUE 128                /* two seconds at 56 ticks */
#define JOY_QUEUE    64
#define CTRL_QUEUE   32
#define CTRL_BYTES   16

/* ------------------------------------------------------------- state ---- */

static int      g_started;              /* between startMP and endMP */
static int      g_session;              /* the link opened during this start */
static int      g_host;                 /* this side runs the fight */
static int      g_dropped;              /* the link went away mid-session */
static int      g_gone_screen;          /* 0x2c still to be shown */

static double   g_last_rx, g_last_tx, g_grace_until;
static int      g_hb_on;
static double   g_hb_timeout;

static int      g_ready_sent, g_peer_ready;

static SPRITES  g_spr[SPRITE_QUEUE];
static int      g_spr_head, g_spr_len;
static int32_t  g_joy[JOY_QUEUE];
static int      g_joy_head, g_joy_len;
static uint8_t  g_ctrl[CTRL_QUEUE][CTRL_BYTES];
static int      g_ctrl_head, g_ctrl_len;

static long     g_pumps;                /* heartbeatUpdate calls: "a frame" */
static long     g_dedup_pump = -1;
static int      g_dedup_type, g_dedup_value;
static int      g_peer_set_pause;       /* GamePaused is ours to clear */
static int      g_menu4_pending;
static int      g_fight_ended;          /* this layer already ended the fight */

static double now(void) { return plat_time(); }

static void put32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }
static int32_t get32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

static void send_raw(const uint8_t *p, int len)
{
    if (g_session && !g_dropped && mpnet_send(p, len))
        g_last_tx = now();
}

static void send_ints(int type, int n, int32_t a, int32_t b)
{
    uint8_t p[1 + 8];

    p[0] = (uint8_t)type;
    if (n > 0) put32(p + 1, a);
    if (n > 1) put32(p + 5, b);
    send_raw(p, 1 + 4 * n);
}

/* The front end sends its menu and pause packets three times in a row: over
 * GKSession that was redundancy against loss. Over a reliable channel the
 * copies would arrive as three separate requests -- three pops instead of
 * one -- so a repeat of the same packet within the same frame is dropped. */
static int repeated(int type, int value)
{
    if (g_dedup_pump == g_pumps && g_dedup_type == type && g_dedup_value == value)
        return 1;
    g_dedup_pump = g_pumps;
    g_dedup_type = type;
    g_dedup_value = value;
    return 0;
}

static void queues_clear(void)
{
    g_spr_head = g_spr_len = 0;
    g_joy_head = g_joy_len = 0;
}

static void session_reset(void)
{
    g_session = g_host = g_dropped = g_gone_screen = 0;
    g_ready_sent = g_peer_ready = 0;
    g_hb_on = 0;
    g_peer_set_pause = 0;
    g_menu4_pending = 0;
    g_fight_ended = 0;
    g_ctrl_head = g_ctrl_len = 0;
    queues_clear();
}

/* dummyObjects (0x0010de84) is two linked records, the first pointing at the
 * second: the image holds that link as the iOS address 0x0010de94, which
 * means nothing here. The guest draws it until the first list arrives. */
static void relocate_dummy_objects(void)
{
    *(void **)(void *)dummyObjects = dummyObjects + 16;
    *(void **)(void *)(dummyObjects + 16) = NULL;
}

/* ------------------------------------------------------------ receiving ---- */

static void on_sprites(const uint8_t *p, int len)
{
    SPRITES *s;
    int n;

    if (len < 1 + 12 + 24 + EVENT_BYTES)
        return;
    n = get32(p + 1);
    if (n < 0 || n > MAX_NODES || len != 1 + 12 + 24 + EVENT_BYTES + n * 12)
        return;
    if (g_spr_len == SPRITE_QUEUE) {    /* a stalled guest: keep the newest */
        g_spr_head = (g_spr_head + 1) % SPRITE_QUEUE;
        g_spr_len--;
    }
    s = &g_spr[(g_spr_head + g_spr_len++) % SPRITE_QUEUE];
    s->n = n;
    s->flags = get32(p + 5);
    s->front = get32(p + 9);
    memcpy(s->pos, p + 13, 24);
    memcpy(s->events, p + 37, EVENT_BYTES);
    memcpy(s->nodes, p + 37 + EVENT_BYTES, (size_t)n * 12);
}

static void on_joy(int32_t word)
{
    if (g_joy_len == JOY_QUEUE) {
        g_joy_head = (g_joy_head + 1) % JOY_QUEUE;
        g_joy_len--;
    }
    g_joy[(g_joy_head + g_joy_len++) % JOY_QUEUE] = word;
}

/* One message off the link. Sprite lists, joystick words and the barrier go
 * where they belong now; the rest waits for the end of the frame. */
static void dispatch(const uint8_t *p, int len)
{
    if (len < 1)
        return;
    g_last_rx = now();
    switch (p[0]) {
    case MPK_PING:
        return;
    case MPK_SPRITES:
        on_sprites(p, len);
        return;
    case MPK_JOY:
        if (len == 5)
            on_joy(get32(p + 1));
        return;
    case MPK_READY:
        /* The channel is ordered: anything queued before the peer's READY
         * belongs to the match before this one. */
        queues_clear();
        g_peer_ready++;
        *(float *)(void *)startTime = 1.0f;     /* "the other side is here" */
        return;
    default:
        if (len > CTRL_BYTES || g_ctrl_len == CTRL_QUEUE)
            return;
        memset(g_ctrl[(g_ctrl_head + g_ctrl_len) % CTRL_QUEUE], 0, CTRL_BYTES);
        memcpy(g_ctrl[(g_ctrl_head + g_ctrl_len) % CTRL_QUEUE], p, (size_t)len);
        g_ctrl_len++;
    }
}

static void pump(void)
{
    uint8_t buf[512];
    int n;

    if (!g_session)
        return;
    while ((n = mpnet_recv(buf, sizeof buf)) != 0)
        if (n > 0)
            dispatch(buf, n);
}

static void apply_pause(int32_t s)
{
    int screen = CurrentTask == TASK_FIGHT
              || (CurrentTask == TASK_FE && FE_CurrentTask == FE_VERSUS);

    otherPlayerPaused = s != 0;
    if (s != 0) {
        /* The "other player paused" panels are drawn by the pause menu,
         * which only runs while GamePaused is set (UpdateInGamePauseMenu
         * returns through TogglePauseMenu otherwise), and the host must
         * stop stepping the engine. */
        if (screen && GamePaused == 0) {
            GamePaused = 1;
            g_peer_set_pause = 1;
        }
    } else if (g_peer_set_pause) {
        /* Only a pause the peer caused is the peer's to end. */
        GamePaused = 0;
        g_peer_set_pause = 0;
    }
}

/* The peer left the fight. A natural end (a side reached WinsNeeded) needs
 * nothing: this side's own round logic gets there from the same state. Left
 * through the pause menu's EXIT, the match is over for this side too -- as a
 * win, the same way the EXIT path ends it as a loss for the one who left
 * (GameCode.c, UpdateInGamePauseMenu's sel == 2). A port decision: what the
 * iPhone did on receiving a quit is not in decomp/. */
static void end_fight_as_win(void)
{
    if (g_fight_ended || CurrentTask != TASK_FIGHT || *FE_FadeAddP != 0.0f)
        return;
    g_fight_ended       = 1;
    GamePaused          = 0;
    PauseMenuAreYouSure = 0;
    RoundHasEnded       = 0;
    DontQuitAfterFade   = 0;
    g_peer_set_pause    = 0;
    otherPlayerPaused   = 0;
    QuitAsWin();
}

static void apply_quit(int natural)
{
    if (CurrentTask == TASK_FE && FE_CurrentTask == FE_POSTMATCH) {
        WaitForOpponent = 0;
        return;
    }
    if (!natural)
        end_fight_as_win();
}

/* Front-end packets, applied in arrival order once the front end is the
 * running task: the pops write FE_FadeAdd, which in a fight is the fight's
 * own fade (Task_GameMain), so they must not land there. */
static int apply_ctrl(const uint8_t *p)
{
    int32_t a = get32(p + 1), b = get32(p + 5);

    switch (p[0]) {
    case MPK_CHAR:
        opponentCharacter = a;
        processCharacterSelectedPacket();
        return 1;
    case MPK_PLAY:
        opponentPressedPlay = 1;
        return 1;
    case MPK_FEMENU:
        if (CurrentTask != TASK_FE)
            return 0;
        if (a >= 0) {
            switchToFETask(a);          /* the deferred push; 0x2c guards itself */
        } else if (a == -1) {           /* rematch: back to VS (summary, choice 1) */
            *requestedLevel = getRandomLevel();
            PopFETaskDeferred();
            resetCharacterSelection();
            enableHeartbeat(3);
        } else if (a == -2) {           /* character select (choice 2) */
            PopFETaskDeferred2();
            resetCharacterSelection();
            enableHeartbeat(3);
        }
        return 1;
    case MPK_MENU:
        /* The VS countdown is the host's: the guest's clock only clamps at
         * 0 and its hand-off waits on a fade only the host starts. Taken
         * only on the VS screen -- switchToTask guards nothing but
         * CurrentTask != 4, and a late copy in the loading screen would run
         * Task_FEDestroy a second time. */
        if (a == 4)
            g_menu4_pending = 1;
        return 1;
    case MPK_GENERIC:
        if (CurrentTask == TASK_FE && FE_CurrentTask == FE_POSTMATCH)
            WaitForOpponent = 0;
        return 1;
    case MPK_LEVEL:
        /* The host has the last word on the arena (preprocessPreloadKode).
         * Never once the fight runs: Task_GameMain ends a fight whose
         * LevelSelect no longer matches the one it loaded. */
        if (!g_host && CurrentTask != TASK_FIGHT && CurrentTask != TASK_SYNC
            && CurrentTask != 7)
            LevelSelect = a;
        return 1;
    case MPK_KODE:
        /* Both sides hold all six digits; each edits its own and sends the
         * digit's absolute index and value. */
        if (a >= 0 && a < 10) {
            KodeSelector[a] = b;
            KodeSelectorParticle[a] = 0.001f;
            checkIfKode();
        }
        return 1;
    case MPK_TIMER:
        if (!g_host) {
            vsScreenTimer = (float)a;
            VSWait = (float)b;
        }
        return 1;
    case MPK_PAUSE:
        apply_pause(a);
        return 1;
    case MPK_QUIT:
        apply_quit(a);
        return 1;
    }
    return 1;                           /* unknown: dropped */
}

static void drain_ctrl(void)
{
    while (g_ctrl_len > 0) {
        if (!apply_ctrl(g_ctrl[g_ctrl_head]))
            break;                      /* waits for the front end */
        g_ctrl_head = (g_ctrl_head + 1) % CTRL_QUEUE;
        g_ctrl_len--;
    }
    if (g_menu4_pending && CurrentTask == TASK_FE && FE_CurrentTask == FE_VERSUS) {
        g_menu4_pending = 0;
        switchToTask(4);
        disableHeartbeat();             /* as the host's own hand-off does */
    }
}

/* The guest's joystick, one word a frame: a tap shorter than the network's
 * jitter arrives in a burst with its release, and applying the burst at once
 * would lose it. Two a frame once a backlog builds, so it cannot grow. */
static void feed_joystick(void)
{
    int k, steps = g_joy_len > 6 ? 2 : 1;

    while (steps-- > 0 && g_joy_len > 0) {
        k = (int)(currentMPJoystickData ^ 1);
        mpOpponentJoystickInput[k * 2]     = g_joy[g_joy_head];
        mpOpponentJoystickInput[k * 2 + 1] = 60;        /* the guest's fps */
        currentMPJoystickData = k;
        g_joy_head = (g_joy_head + 1) % JOY_QUEUE;
        g_joy_len--;
    }
}

/* The link is gone while a session is on. Out of a fight, the front end goes
 * to its Disconnected screen (0x2c), whose button rebuilds the lobby
 * (Task_FEMain, PendingPop 0x29a). In a fight, the fight ends as a win and
 * 0x2c follows once the front end is back. While loading, syncGame lets the
 * load finish first. Nothing in decomp/ pushes 0x2c: this layer always did. */
static void on_dropped(void)
{
    if (g_dropped)
        return;
    g_dropped = 1;
    g_gone_screen = GameMode == 1;
    mpnet_stop();                       /* a silent peer: hang up on it too */
    otherPlayerPaused = 0;
    if (g_peer_set_pause) {
        GamePaused = 0;
        g_peer_set_pause = 0;
    }
    printf("multiplayer: link lost\n");
}

static void show_gone(void)
{
    if (!g_gone_screen)
        return;
    if (CurrentTask == TASK_FIGHT) {
        end_fight_as_win();
    } else if (CurrentTask == TASK_FE && FE_CurrentTask != FE_DISCONNECTED) {
        PushFETask(FE_DISCONNECTED);
        g_gone_screen = 0;
    }
}

/* -------------------------------------------------- the session's state ---- */

void startMP(void)
{
    static int relocated;

    if (!relocated) {
        relocate_dummy_objects();
        relocated = 1;
    }
    if (g_started && !g_dropped)
        return;                         /* _startMP: nothing once it exists */
    session_reset();
    g_started = 1;
    mpnet_start(MP_WIRE);
    if (mpnet_state() == MPNET_CLOSED)
        mpnet_relist();
}

void endMP(void)
{
    if (!g_started)
        return;
    mpnet_stop();
    session_reset();
    g_started = 0;
}

void restartConnection(void)
{
    if (g_started) {
        session_reset();
        mpnet_relist();
    }
}

long isMPConnected(void)
{
    return g_session && !g_dropped;
}

long isParent(void)             { return GameMode == 1 && g_session && g_host; }
long isParentBasedOnSpeed(void) { return isParent(); }

/* Busy: an invitation is out or the link is being set up. The lobby draws
 * its ellipsis and refuses another pick meanwhile. */
long isWorking(void)
{
    return g_started && !g_session && mpnet_state() == MPNET_CONNECTING;
}

/* FE_Task_Multiplayer writes peer i's name into peerNames row i + 1 of eight
 * and only lets indices 0..4 be picked: more than five would draw past the
 * rows it can use. */
long getNumberOfPeers(void)
{
    int n;

    if (!g_started || g_session)
        return 0;
    n = mpnet_peer_count();
    return n > 5 ? 5 : n;
}

/* The return value is the row's "unavailable" flag (drawn dark, not
 * pickable); every listed player is available. */
long getPeerNumber(long index, char *out, long size)
{
    if (out && size > 0) {
        out[0] = 0;
        if (g_started)
            mpnet_peer_name((int)index, out, (int)size);
    }
    return 0;
}

void connectToPeer(long index)
{
    if (g_started && !g_session)
        mpnet_invite((int)index);
}

/* Once a lobby frame. An invitation is accepted while this player is in the
 * lobby: being on that screen is what being listed means, and there is no
 * accept dialog anywhere in the decompiled front end -- the iPhone's, if it
 * had one, was GameKit's. Back in character select leaves again. */
void updateLobbyHeartBeat(void)
{
    char name[64];

    if (g_started && !g_session && mpnet_invitation(name, sizeof name)) {
        printf("multiplayer: %s challenges you\n", name);
        mpnet_answer(1);
    }
}

void doFPSExchange(void) { }             /* the roles are the lobby's */

/* ----------------------------------------------------------- heartbeat ---- */

void enableHeartbeat(long seconds)
{
    g_hb_on = 1;
    g_hb_timeout = seconds < 4 ? 4.0 : (double)seconds;    /* jitter, tabs */
    g_last_rx = now();
}

void disableHeartbeat(void)          { g_hb_on = 0; }
long isHeartbeatOn(void)             { return g_hb_on; }

/* Before blocking work: the peer may hear nothing from here for `seconds`. */
void heartbeatSetIncoming(long seconds)
{
    double until = now() + (double)seconds;
    if (until > g_grace_until)
        g_grace_until = until;
    g_last_rx = now();
}

void heartbeatSend(void)
{
    send_ints(MPK_PING, 0, 0, 0);
}

/* Every frame, in every task (GameCodeMain), and again at the end of every
 * front-end frame: time-based, and cheap with no session. */
void heartbeatUpdate(void)
{
    double t;
    int st;

    g_pumps++;
    if (!g_started)
        return;

    st = mpnet_state();
    if (!g_session && st == MPNET_CONNECTED) {
        g_session = 1;
        g_host = mpnet_is_host();
        g_last_rx = g_last_tx = now();
        queues_clear();
        printf("multiplayer: connected, %s\n", g_host ? "host" : "guest");
    }
    if (!g_session)
        return;

    pump();
    t = now();
    if (!g_dropped && (st != MPNET_CONNECTED
                       || (g_hb_on && t > g_grace_until
                           && t - g_last_rx > g_hb_timeout)))
        on_dropped();
    if (g_dropped) {
        show_gone();
        return;
    }
    if (t - g_last_tx > 0.25)
        heartbeatSend();
    drain_ctrl();
    if (g_host)
        feed_joystick();
}

/* ---------------------------------------------------------------- sync ---- */

/* Task_MultiplayerSync's barrier: 1 once both sides have loaded. Called once
 * from Task_GameInit as the load ends, then every frame of task 9. A lost
 * link lets the load through, and the fight then ends at once. */
long syncGame(long frame)
{
    (void)frame;
    if (!g_session || g_dropped)
        return 1;
    if (!g_ready_sent) {
        send_ints(MPK_READY, 0, 0, 0);
        g_ready_sent = 1;
    }
    pump();
    if (g_peer_ready > 0) {
        g_peer_ready--;
        g_ready_sent = 0;
        g_fight_ended = 0;              /* a new fight starts */
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------- sending ---- */

void sendCharacterPacket(long who)      { send_ints(MPK_CHAR, 1, (int32_t)who, 0); }
void sendPlayPacket(void)               { send_ints(MPK_PLAY, 0, 0, 0); }
void sendGenericPacket(long a, long b)  { send_ints(MPK_GENERIC, 2, (int32_t)a, (int32_t)b); }
void sendLevelPacket(void)              { send_ints(MPK_LEVEL, 1, (int32_t)LevelSelect, 0); }

void sendFEMenuPacket(long item)
{
    if (!repeated(MPK_FEMENU, (int)item))
        send_ints(MPK_FEMENU, 1, (int32_t)item, 0);
}

/* The host's VS countdown ran out. Its own hand-off decides the arena after
 * a 30-frame fade, but the guest starts loading as soon as this arrives and
 * reads LevelSelect two steps in: decide it now so the level packet goes out
 * first on the ordered channel. Both calls are idempotent; the hand-off and
 * GameInit_LoadABit's step 0 make them again with the same result. */
void sendMenuPacket(long which)
{
    if (which == 4 && g_host) {
        checkIfKode();
        preprocessPreloadKode();
    }
    send_ints(MPK_MENU, 1, (int32_t)which, 0);
}

void sendPause(long state)
{
    if (!repeated(MPK_PAUSE, (int)state))
        send_ints(MPK_PAUSE, 1, (int32_t)state, 0);
}

/* `natural` tells the peer whether a side reached WinsNeeded (its own round
 * logic will follow) or this side left through the pause menu. */
void sendQuit(void)
{
    int natural = RoundWins[0] >= WinsNeeded || RoundWins[1] >= WinsNeeded;
    send_ints(MPK_QUIT, 1, natural, 0);
}

/* The digit `index` of the half at `digits`; sent as its absolute index into
 * KodeSelector so the receiver writes the same cell. */
void sendKodePacket(const int *digits, long index)
{
    long abs_index = (digits - KodeSelector) + index;

    if (abs_index >= 0 && abs_index < 10)
        send_ints(MPK_KODE, 2, (int32_t)abs_index, KodeSelector[abs_index]);
}

void sendTimerPacket(long timer, long wait)
{
    send_ints(MPK_TIMER, 2, (int32_t)timer, (int32_t)wait);
}

void sendJoystickInputPacket(long a, long bits)
{
    (void)bits;                         /* the same word, twice, at the call */
    send_ints(MPK_JOY, 1, (int32_t)a, 0);
}

/* The host's tick: the display list mk3_update just built, the positions,
 * the tick's events and the round-live flag. `objects` is the list's head
 * (GameObjects' value); its records are contiguous and linked in order
 * (mk3_update), so the records travel without their link words and the
 * guest relinks them in its own buffer.
 *
 * G+0x474 -- which fighter is drawn in front, read by RenderLevelPlayers
 * through mk3_who_in_front -- is the engine's alone, and the guest, which
 * never runs it, would keep its own start-of-match coin flip. It rides
 * along: a port addition, nothing in decomp/ says the iPhone sent it. */
void sendSpriteListPacket(void *objects, long a, long b)
{
    uint8_t p[1 + 12 + 24 + EVENT_BYTES + MAX_NODES * 12];
    const uint8_t *o = (const uint8_t *)objects;
    int n = 0, len;

    (void)a;
    (void)b;
    if (!g_session || g_dropped || !o)
        return;
    while (o && n < MAX_NODES) {
        memcpy(p + 1 + 12 + 24 + EVENT_BYTES + n * 12, o + 4, 12);
        n++;
        o = *(const uint8_t *const *)(const void *)o;
    }
    p[0] = MPK_SPRITES;
    put32(p + 1, n);
    put32(p + 5, getTransferableFlags());
    put32(p + 9, G ? *(const int32_t *)(const void *)((const char *)G + WHO_IN_FRONT) : 0);
    memcpy(p + 13, Player1Pos, 12);
    memcpy(p + 25, Player2Pos, 12);
    memcpy(p + 37, MKEventQueue, EVENT_BYTES);
    len = 1 + 12 + 24 + EVENT_BYTES + n * 12;
    send_raw(p, len);
}

/* The guest's tick: the next list into the slot not being drawn, relinked,
 * with its positions and events where UpdateArcadeCode copies them from.
 * 1 while lists remain, in arrival order. */
long setNextSpritesAndEvents(void)
{
    SPRITES *s;
    char *slot, *pkt;
    int k, i;

    pump();
    if (g_spr_len == 0)
        return 0;
    s = &g_spr[g_spr_head];
    g_spr_head = (g_spr_head + 1) % SPRITE_QUEUE;
    g_spr_len--;

    k = (int)(currentMPSpriteList ^ 1);
    slot = mpSpriteList + k * SPRITE_STRIDE;
    for (i = 0; i < s->n; i++) {
        char *rec = slot + i * 16;
        *(void **)(void *)rec = (i + 1 < s->n) ? rec + 16 : NULL;
        memcpy(rec + 4, s->nodes[i], 12);
    }
    pkt = mpEventQueue + k * EVENTQ_STRIDE;
    memcpy(pkt, s->pos, 24);
    memcpy(pkt + 0x18, s->events, EVENT_BYTES);

    setTransferableFlags(s->flags);
    if (G)
        *(int32_t *)(void *)((char *)G + WHO_IN_FRONT) = s->front;

    currentMPSpriteList = k;
    SpritelistReceived = 1;
    return 1;
}

/* ------------------------------------------------------------- testing ---- */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

extern int   CharacterSelected, CharacterConfirmed, playerCharacter;
extern long  pressedPlay, GameMode;
extern float FE_Fade, FE_FadeAdd, Character_SelectWait;
extern float limeTouchScreenX[10], limeTouchScreenY[10], limeLastTouchScreenX[10];

/* The state a test of a match needs to see, as one line of JSON:
 * Module._umk3_debug() from the page. Read-only; nothing calls it. */
EMSCRIPTEN_KEEPALIVE const char *umk3_debug(void)
{
    static char buf[512];

    snprintf(buf, sizeof buf,
             "{\"task\":%d,\"fe\":%d,\"mode\":%ld,\"host\":%d,\"session\":%d,"
             "\"dropped\":%d,\"sel\":%d,\"confirmed\":%d,\"mine\":%d,\"theirs\":%d,"
             "\"pressedPlay\":%ld,\"theyPressed\":%ld,\"fade\":%.3f,\"fadeAdd\":%.4f,"
             "\"selWait\":%.2f,\"touch\":[%.0f,%.0f,%.0f,%.0f],\"lastTouch0\":%.0f,"
             "\"sprites\":%d,\"joys\":%d,\"peerReady\":%d,\"paused\":%ld,\"otherPaused\":%ld}",
             CurrentTask, FE_CurrentTask, GameMode, g_host, g_session, g_dropped,
             CharacterSelected, CharacterConfirmed, playerCharacter,
             opponentCharacter, pressedPlay, opponentPressedPlay, FE_Fade,
             FE_FadeAdd, Character_SelectWait, limeTouchScreenX[0],
             limeTouchScreenY[0], limeTouchScreenX[1], limeTouchScreenY[1],
             limeLastTouchScreenX[0], g_spr_len, g_joy_len, g_peer_ready,
             GamePaused, otherPlayerPaused);
    return buf;
}
#endif
