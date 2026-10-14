/*
 * test_mp_session.c -- runtime/mp_session.c over a loopback link.
 *
 *   i686-w64-mingw32-clang -std=gnu11 -Wall -Iruntime \
 *       tests/test_mp_session.c runtime/mp_session.c -o test_mp_session.exe
 *   (or emcc ... -o test_mp_session.js && node test_mp_session.js)
 *
 * What one side sends, the same process receives: mp_net.h is implemented
 * here as a queue, and the game's globals and the decompiled functions the
 * layer calls are stand-ins that record what happened. So every packet is
 * checked end to end -- encoded, carried, decoded and applied to the state
 * the decompiled consumers read -- without a browser or the game's data.
 * The real transport is tests/web_mp_link.c's; the real game is a human's.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp_net.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ------------------------------------------------- the loopback "radio" ---- */

static int    net_state = MPNET_OFF, net_host, net_invited = -1, net_inviting;
static uint8_t q[256][512];
static int    qlen[256], qhead, qcount, sent_count;

void mpnet_start(unsigned build)  { (void)build; net_state = MPNET_LOBBY; }
void mpnet_stop(void)             { net_state = MPNET_OFF; }
void mpnet_relist(void)           { net_state = MPNET_LOBBY; }
int  mpnet_state(void)            { return net_state; }
int  mpnet_peer_count(void)       { return 7; }
int  mpnet_peer_name(int i, char *out, int n) { snprintf(out, (size_t)n, "PEER%d", i); return 1; }
int  mpnet_invite(int i)          { net_invited = i; net_state = MPNET_CONNECTING; return 1; }
int  mpnet_invitation(char *out, int n) { if (!net_inviting) return 0; snprintf(out, (size_t)n, "RIVAL"); return 1; }
void mpnet_answer(int accept)     { net_inviting = 0; if (accept) net_state = MPNET_CONNECTING; }
int  mpnet_is_host(void)          { return net_host; }
void mpnet_partner_name(char *out, int n) { snprintf(out, (size_t)n, "RIVAL"); }

int mpnet_send(const void *p, int len)
{
    if (net_state != MPNET_CONNECTED || qcount == 256 || len > 512)
        return 0;
    memcpy(q[(qhead + qcount) % 256], p, (size_t)len);
    qlen[(qhead + qcount) % 256] = len;
    qcount++;
    sent_count++;
    return 1;
}

int mpnet_recv(void *p, int cap)
{
    int len;
    if (qcount == 0)
        return 0;
    len = qlen[qhead];
    if (len > cap) { qhead = (qhead + 1) % 256; qcount--; return -1; }
    memcpy(p, q[qhead], (size_t)len);
    qhead = (qhead + 1) % 256;
    qcount--;
    return len;
}

static double fake_time;
double plat_time(void) { return fake_time; }

/* ---------------------------------------------------- the game, stood in ---- */

long  GameMode, GamePaused, otherPlayerPaused, WaitForOpponent, opponentPressedPlay;
int   CurrentTask, FE_CurrentTask, opponentCharacter = -1;
long  LevelSelect;
int   KodeSelector[10];
float KodeSelectorParticle[10];
float vsScreenTimer, VSWait;
long  RoundWins[2], WinsNeeded = 2;
long  PauseMenuAreYouSure, RoundHasEnded, DontQuitAfterFade;
static float fade_add;
float *FE_FadeAddP = &fade_add;
static long req_level = 7, start_time;
long *requestedLevel = &req_level;
long *startTime = &start_time;
static uint8_t g_block[0x500];
void *G = g_block;

/* read only by umk3_debug(), the browser build's test readout */
int   CharacterSelected = -1, CharacterConfirmed = -1, playerCharacter = -1;
long  pressedPlay;
float FE_Fade, FE_FadeAdd, Character_SelectWait;
float limeTouchScreenX[10], limeTouchScreenY[10], limeLastTouchScreenX[10];

long  SpritelistReceived, currentMPSpriteList, currentMPJoystickData;
long  mpOpponentJoystickInput[4], opponentFPS2;
char  mpSpriteList[0x140], mpEventQueue[0x1b0];
static long events_store[21];
long *MKEventQueue = events_store;
float Player1Pos[3], Player2Pos[3];
char  dummyObjects[32];

static int calls_switch_task, last_switch_task, calls_push_deferred, last_push_deferred;
static int calls_char_processed, calls_pop1, calls_pop2, calls_reset_select;
static int calls_push, last_push, calls_kode_check, calls_quit_win, calls_preload;
static long transferable;

void switchToTask(int t)            { calls_switch_task++; last_switch_task = t; if (t == 4) CurrentTask = 4; }
void switchToFETask(int t)          { calls_push_deferred++; last_push_deferred = t; }
void processCharacterSelectedPacket(void) { calls_char_processed++; }
int  getTransferableFlags(void)     { return (int)transferable; }
void setTransferableFlags(long f)   { transferable = f & 1; }
void PushFETask(int t)              { calls_push++; last_push = t; FE_CurrentTask = t; }
void PopFETaskDeferred(void)        { calls_pop1++; }
void PopFETaskDeferred2(void)       { calls_pop2++; }
void resetCharacterSelection(void)  { calls_reset_select++; }
int  getRandomLevel(void)           { return 3; }
void checkIfKode(void)              { calls_kode_check++; }
void preprocessPreloadKode(void)    { calls_preload++; }
void QuitAsWin(void)                { calls_quit_win++; fade_add = -0.033f; }

/* the layer's own API */
void startMP(void); void endMP(void); long isMPConnected(void);
long isParent(void); long isParentBasedOnSpeed(void); long isWorking(void);
long getNumberOfPeers(void); long getPeerNumber(long, char *, long);
void connectToPeer(long); void updateLobbyHeartBeat(void);
void enableHeartbeat(long); void disableHeartbeat(void); void heartbeatUpdate(void);
long syncGame(long);
void sendCharacterPacket(long); void sendPlayPacket(void); void sendFEMenuPacket(long);
void sendMenuPacket(long); void sendGenericPacket(long, long); void sendLevelPacket(void);
void sendPause(long); void sendQuit(void); void sendKodePacket(const int *, long);
void sendTimerPacket(long, long); void sendJoystickInputPacket(long, long);
void sendSpriteListPacket(void *, long, long); long setNextSpritesAndEvents(void);

/* Connect: the lobby, then the link opening as the host or the guest. */
static void connect_as(int host)
{
    endMP();
    qhead = qcount = 0;                 /* a new link starts empty */
    net_host = host;
    GameMode = 0;
    startMP();
    net_state = MPNET_CONNECTED;
    heartbeatUpdate();
    GameMode = 1;                       /* FE_Task_Multiplayer's step */
}

static void frame(void) { fake_time += 1.0 / 60.0; heartbeatUpdate(); }

int main(void)
{
    char name[64];

    /* ---- the lobby ---- */
    startMP();
    CHECK(*(void **)(void *)dummyObjects == dummyObjects + 16);   /* relocated */
    CHECK(getNumberOfPeers() == 5);                 /* capped at the rows */
    getPeerNumber(2, name, sizeof name);
    CHECK(strcmp(name, "PEER2") == 0);
    CHECK(!isMPConnected() && !isParent() && !isParentBasedOnSpeed());
    connectToPeer(3);
    CHECK(net_invited == 3 && isWorking());
    net_inviting = 1;
    updateLobbyHeartBeat();
    CHECK(net_inviting == 0);                       /* accepted in the lobby */

    /* ---- roles ---- */
    connect_as(1);
    CHECK(isMPConnected() && isParent() && isParentBasedOnSpeed());
    CHECK(getNumberOfPeers() == 0);
    GameMode = 0;
    CHECK(!isParent());                             /* 0 outside GameMode 1 */
    GameMode = 1;

    /* ---- select screen ---- */
    CurrentTask = 3; FE_CurrentTask = 0x2a;
    sendCharacterPacket(12);
    sendPlayPacket();
    frame();
    CHECK(opponentCharacter == 12 && calls_char_processed == 1);
    CHECK(opponentPressedPlay == 1);

    /* three copies in a frame are one request */
    sendFEMenuPacket(0x2b); sendFEMenuPacket(0x2b); sendFEMenuPacket(0x2b);
    frame();
    CHECK(calls_push_deferred == 1 && last_push_deferred == 0x2b);

    /* ---- VS: kodes, timer, the host's go ---- */
    FE_CurrentTask = 0x2b;
    KodeSelector[8] = 4;
    sendKodePacket(&KodeSelector[7], 1);            /* the guest's half */
    KodeSelector[8] = 0;
    frame();
    CHECK(KodeSelector[8] == 4 && calls_kode_check == 1);

    connect_as(0);                                  /* the guest's view */
    CurrentTask = 3; FE_CurrentTask = 0x2b;
    sendTimerPacket(321, 17);
    LevelSelect = 9;
    sendLevelPacket();
    LevelSelect = 0;
    sendMenuPacket(4);
    CHECK(calls_preload == 0);                      /* only the host decides */
    frame();
    CHECK(vsScreenTimer == 321.0f && VSWait == 17.0f);
    CHECK(LevelSelect == 9);
    CHECK(calls_switch_task == 1 && last_switch_task == 4 && CurrentTask == 4);
    sendMenuPacket(4);                              /* a late copy */
    frame();
    CHECK(calls_switch_task == 1);                  /* not on VS: ignored */

    /* ---- the fight barrier ---- */
    CurrentTask = 5;
    CHECK(syncGame(1) == 1);                        /* loopback: our own READY */
    CHECK(*(float *)(void *)startTime == 1.0f);

    /* ---- the guest's stick reaches the host one word a frame ---- */
    connect_as(1);
    CurrentTask = 6;
    sendJoystickInputPacket(0x21, 0x21);
    sendJoystickInputPacket(0x00, 0x00);            /* a tap and its release */
    frame();
    CHECK(mpOpponentJoystickInput[currentMPJoystickData * 2] == 0x21);
    CHECK(mpOpponentJoystickInput[currentMPJoystickData * 2 + 1] == 60);
    frame();
    CHECK(mpOpponentJoystickInput[currentMPJoystickData * 2] == 0x00);

    /* ---- the host's tick, drawn by the guest ---- */
    {
        uint8_t recs[3][16];
        int i;

        memset(recs, 0, sizeof recs);
        for (i = 0; i < 3; i++) {
            *(void **)(void *)recs[i] = i < 2 ? recs[i + 1] : NULL;
            recs[i][4] = (uint8_t)(10 + i);         /* x */
            recs[i][8] = (uint8_t)(20 + i);         /* animation */
            recs[i][15] = (uint8_t)(30 + i);
        }
        Player1Pos[0] = 1.5f; Player2Pos[2] = -2.5f;
        events_store[0] = 1; events_store[1] = 0x1234;
        transferable = 1;
        *(int32_t *)(void *)(g_block + 0x474) = 1;

        connect_as(0);                  /* a new link: send it again */
        sendSpriteListPacket(recs, 0, 0);
        transferable = 0;
        *(int32_t *)(void *)(g_block + 0x474) = 0;
        CHECK(setNextSpritesAndEvents() == 1);
        CHECK(setNextSpritesAndEvents() == 0);
        {
            char *slot = mpSpriteList + currentMPSpriteList * 160;
            char *pkt = mpEventQueue + currentMPSpriteList * 216;
            CHECK(*(void **)(void *)slot == slot + 16);
            CHECK(*(void **)(void *)(slot + 16) == slot + 32);
            CHECK(*(void **)(void *)(slot + 32) == NULL);
            CHECK((uint8_t)slot[16 + 4] == 11 && (uint8_t)slot[32 + 8] == 22
                  && (uint8_t)slot[32 + 15] == 32);
            CHECK(*(float *)(void *)pkt == 1.5f && *(float *)(void *)(pkt + 20) == -2.5f);
            CHECK(*(long *)(void *)(pkt + 0x18) == 1 && *(long *)(void *)(pkt + 0x1c) == 0x1234);
            CHECK(transferable == 1);               /* G+0x44e: ReadControls runs */
            CHECK(*(int32_t *)(void *)(g_block + 0x474) == 1);
            CHECK(SpritelistReceived == 1);
        }
    }

    /* ---- pause: only a pause the peer caused is the peer's to end ---- */
    CurrentTask = 6; GamePaused = 0;
    sendPause(1); sendPause(1); sendPause(1);
    frame();
    CHECK(otherPlayerPaused == 1 && GamePaused == 1);
    sendPause(0);
    frame();
    CHECK(otherPlayerPaused == 0 && GamePaused == 0);
    GamePaused = 2;                                 /* our own moves list */
    sendPause(1);
    frame();
    sendPause(0);
    frame();
    CHECK(GamePaused == 2);

    /* ---- quits ---- */
    fade_add = 0.0f; RoundWins[0] = 2;
    sendQuit();                                     /* natural: nothing forced */
    frame();
    CHECK(calls_quit_win == 0);
    RoundWins[0] = 0;
    sendQuit();                                     /* left through EXIT */
    frame();
    CHECK(calls_quit_win == 1);
    sendQuit();
    frame();
    CHECK(calls_quit_win == 1);                     /* once */

    /* post-match: the peer's presence frees the host */
    CurrentTask = 3; FE_CurrentTask = 0x28; WaitForOpponent = 1;
    sendGenericPacket(0x3f800000, 0);
    frame();
    CHECK(WaitForOpponent == 0);
    sendFEMenuPacket(-1); sendFEMenuPacket(-1); sendFEMenuPacket(-1);
    frame();
    CHECK(calls_pop1 == 1 && calls_reset_select == 1);
    sendFEMenuPacket(-2);
    frame();
    CHECK(calls_pop2 == 1 && calls_reset_select == 2);

    /* front-end packets wait for the front end */
    CurrentTask = 6; fade_add = 0.0f;
    sendFEMenuPacket(-1);
    frame();
    CHECK(calls_pop1 == 1);
    CurrentTask = 3;
    frame();
    CHECK(calls_pop1 == 2);

    /* ---- a dropped link: the Disconnected screen ---- */
    connect_as(1);
    CurrentTask = 3; FE_CurrentTask = 0x2a;
    net_state = MPNET_CLOSED;
    frame();
    CHECK(!isMPConnected());
    CHECK(calls_push == 1 && last_push == 0x2c);
    frame();
    CHECK(calls_push == 1);

    /* a silent peer, with the heartbeat on */
    connect_as(1);
    CurrentTask = 3; FE_CurrentTask = 0x2b;
    enableHeartbeat(5);
    qcount = 0;
    {
        int i;
        for (i = 0; i < 60 * 3; i++) {              /* three seconds: fine */
            fake_time += 1.0 / 60.0;
            qcount = 0;                             /* swallow our own pings */
            heartbeatUpdate();
        }
        CHECK(isMPConnected());
        for (i = 0; i < 60 * 3; i++) {
            fake_time += 1.0 / 60.0;
            qcount = 0;
            heartbeatUpdate();
        }
        CHECK(!isMPConnected() && last_push == 0x2c && calls_push == 2);
    }

    /* in a fight, a drop ends it as a win, once */
    connect_as(1);
    CurrentTask = 6; fade_add = 0.0f;
    net_state = MPNET_CLOSED;
    frame();
    frame();
    CHECK(calls_quit_win == 2);
    CHECK(syncGame(1) == 1);                        /* loads are let through */

    endMP();
    CHECK(!isMPConnected() && !isParent());

    printf("%s: %d failure(s), %d packets sent\n", failures ? "FAIL" : "PASS",
           failures, sent_count);
    return failures != 0;
}
