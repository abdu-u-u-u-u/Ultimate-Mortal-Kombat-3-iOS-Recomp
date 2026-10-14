# Online multiplayer (browser build)

The iOS game had a two-player mode over GameKit (`GKSession`: Bluetooth or
local WiFi, two iPhones in the same room). Its front end, its versus flow and
its network split of the fight are all in `decomp/`; the layer underneath --
`limeMPSession` and `limeMPGameSpecific`, about 126 Objective-C functions --
was never decompiled, and the native builds answer its 31 entry points "radio
off" (`runtime/gamecode_stubs.c`).

The browser build plays it online. Nothing about the game's own flow is
invented: the lobby is still `FE_Task_Multiplayer`, the select screen is still
0x2a, the countdown is still the VS screen's, and the fight is still split the
way `UpdateArcadeCode` splits it. What is new is the radio.

## The pieces

| Piece | What it does |
|---|---|
| `server/signal.js` | The lobby (who has the multiplayer screen open, by room and build), invitations, and the WebRTC handshake. Carries no game traffic. Runs at `umk3.abdu.cloud/signal`. |
| coturn (`server/deploy/turnserver.conf`) | A relay for the pairs of browsers that cannot reach each other directly (symmetric NAT, mobile hotspots). Credentials are minted per player by `signal.js`. |
| `web/mp_webrtc.js` | The browser's transport: the lobby over a WebSocket, then one reliable, ordered WebRTC data channel to the opponent. Implements `runtime/mp_net.h`. |
| `runtime/mp_session.c` | The 31 functions the game calls, over `mp_net.h`. Built only into the browser game (`UMK3_HAVE_MP`). |

## How a match goes

1. Main menu → *Multiplayer*: `startMP` puts the player in the lobby of their
   room (`?room=` in the page's URL, `public` by default). The lobby lists the
   others with the same screen open, five at most (the screen's limit). The
   room comes from the link alone and is never remembered or made up: a page
   opened from the plain link is in the public lobby, and *Copy invite link*
   copies the link to the lobby the page is in. The online chip names the
   room and says whether the page has reached the lobby server; a dropped
   lobby connection reconnects by itself.
2. A tap on a name invites that player. A player in the lobby accepts
   automatically: being on that screen is what being listed means, and the
   decompiled front end has no accept dialog. Two players who tap each other
   at once are paired once (the lower lobby id accepts).
3. The data channel opens, `isMPConnected` turns true on both sides, and the
   game's own code takes `GameMode` to 1 and both to the select screen.
4. From there the game drives everything with its own packets: picks, *Play*,
   the kode digits, the host's countdown, the arena, pauses, the end of the
   match, rematch or back to select.

**Roles.** The player who accepted hosts. `isParent` and
`isParentBasedOnSpeed` both answer "host" (the iPhone elected the faster
device; two browsers at a fixed 60 Hz have nothing to elect, and the
decompiled code is only consistent when the two agree). The host runs the
fight engine with both joystick words and sends each engine tick's display
list, positions, event queue and round-live flag; the guest draws them. The
host is player 1, the guest player 2, on both screens.

## Packet rules, and where each comes from

What a packet *does* on arrival is read off the decompiled consumers -- the
globals the front end and the fight wait on that nothing in `decomp/` writes,
and five decompiled functions nothing calls (`switchToTask`, `switchToFETask`,
`processCharacterSelectedPacket`, `getTransferableFlags`,
`setTransferableFlags`). The wire format is the port's own: the lobby only
pairs equal builds.

These are port decisions, not read off the game, and the code says so where
it makes them:

- invitations are accepted automatically in the lobby;
- a quit through the pause menu's EXIT ends the other side's fight as a win
  (a match that ended normally needs nothing: both sides get there from the
  same state);
- the heartbeat's timeouts (the game's own, at least four seconds);
- a dropped link ends a fight as a win, then shows the Disconnected screen
  (0x2c), whose button rebuilds the lobby;
- the guest's joystick reaches the host one word a frame, so a tap shorter
  than the network's jitter is not lost in a burst;
- which fighter is drawn in front (`G+0x474`) travels with the sprite list.

## Fixed on the way

Four sites on the never-exercised network paths of `GameCode.c` would have
crashed the first online fight; they are fixed and commented there:

- `Task_GameInit` set `CurrentTask = 6 + 8` (14, an empty `TaskFunctionList`
  slot) where the sync task is 9.
- `UpdateArcadeCode`'s guest stored through `GameObjects[0]` -- through the
  NULL `Task_GameInit` leaves -- where the variable itself is meant (twice),
  and the host sent `GameObjects[0]`, record 0's link, as the list.
- `dummyObjects`' first record links with an iOS address; `startMP` relocates
  it.

These are reasoned from the code's own invariants, not yet re-read at armv7.

Two suspects are left as transcribed, because they need the binary to settle:
the kode-digit ownership test in `FE_Task_Multiplayer_Versus_Screen`
(FrontEnd.c, `mine = isParent() ? ... : (i <= 5)`, which lets the guest edit
the host's digits) and `preprocessPreloadKode`'s host override running after
every kode (achievements.c), which makes stage kodes do nothing online.

## Controllers

A gamepad plays fights and drives the menus (`runtime/game_main.c`,
`pad_poll`): stick or d-pad, X/A/Y/B the punches and kicks, RB block, LB or RT
run, LT special, Start the pause menu, Back the moves list; in the menus the
stick moves a pointer and A taps. In the browser any pad the Gamepad API calls
"standard" works (SDL 2.32's Emscripten mapping); it shows after its first
button press, which is the browser's rule.

## Building

```sh
. toolchain/emsdk/emsdk_env.sh              # Emscripten (see web/README.md)
tools/build_web.sh <your UMK3 .ipa> build-web/game
server/deploy/publish-web.sh build-web/game  # needs ssh access to the host
```

`build_web.sh` reads the binary for the tables' layout only (names, sizes,
addresses, which words are pointers -- `tools/logic_tables.py --layout`); the
build holds none of the game's bytes, and every player's page fills the
tables from that player's own copy (`runtime/umk3_tables_rt.c`). It compiles
the umk3-game target's sources with the browser's platform layer, the
multiplayer layer and the run-time tables, and links with:

- `EMULATE_FUNCTION_POINTER_CASTS`: the fight engine calls most of its code
  through tables of one pointer type whose targets are not all declared that
  way; wasm traps such a call where i686 does not mind.
- `ASYNCIFY` with `ASYNCIFY_IGNORE_INDIRECT`: only the main loop sleeps.
- `INITIAL_MEMORY` 128 MB: about 67 MB of static data.
- IDBFS at `/save` (`web/save_mount.js`): the game's saves survive a reload.

Getting the engine to link for wasm took declarations that i686 tolerated
and wasm-ld does not: two threads `mkreact.c` declared as `char[]`, 49 tables
`mkdrone.c` declared as functions (`funcs_*`, `rpt_*`), and eleven
prototypes whose return type or arity disagreed with the definition. One of
those was a real bug on every platform: `mkdrone.c` tested the return value
of `is_he_short`, which returns nothing and answers in `field5c`.

Verified without the game: the whole program links against stand-ins for the
table symbols, and in a browser the page reads an .ipa, mounts it, starts the
program, refuses a wrong executable with a message, forgets that copy and
restarts with the next one, and keeps the game's saves across reloads.

## Deploying

```
server/deploy/umk3-signal.service    systemd unit for signal.js (127.0.0.1:8830)
server/deploy/umk3.abdu.cloud.nginx  the site: static files, /signal proxied
server/deploy/turnserver.conf        coturn; the secret is set on the server
server/deploy/publish-web.sh         a build into /g/<hash>/, then the root page
```

The relay's URLs name the server's IP: `umk3.abdu.cloud` is proxied by
Cloudflare, which carries HTTP and WebSockets but not TURN. Cloudflare also
holds `.js` for four hours whatever the server sends, which is why every
build is published under a new folder.

## Testing

| Test | Covers |
|---|---|
| `cd server && npm test` | The lobby service, and `web/mp_webrtc.js`'s lobby logic loaded as written into a vm context per browser: accept, decline, crossed invitations. |
| `tests/test_mp_session.c` | `mp_session.c` over a loopback link with the game stood in: every packet end to end, pause ownership, quits, the post-match screen, dropped and silent links. Native and wasm. |
| `tests/web_mp_link.c` | The real transport in two browser tabs: lobby, invitation, data channel, 200 messages each way in order. `?relay=1` forces the TURN relay. |

Verified: all three, the last against the deployed service, both direct and
relay-only, with both tabs on one machine. **Not yet verified:** the build
against a real binary (`tools/build_web.sh` needs one, and nobody on this
branch has had a copy), a match in the game itself, play between two
different networks, and a physical controller.
