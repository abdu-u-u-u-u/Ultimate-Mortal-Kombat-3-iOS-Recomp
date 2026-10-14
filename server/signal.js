#!/usr/bin/env node
/*
 * signal.js -- the lobby and WebRTC signalling service for online multiplayer.
 *
 * The iOS game found its opponent with GameKit: GKSession advertised the
 * device over Bluetooth / local WiFi, listed the peers it could see, and
 * connected to the one the player tapped. A browser can do none of that on its
 * own, so this service stands in for the radio and nothing else:
 *
 *   - the lobby: who has the multiplayer screen open right now (startMP .. endMP
 *     in the game), so FE_Task_Multiplayer can list them the way it listed
 *     nearby iPhones;
 *   - the invitation: one player taps another, the other accepts or declines;
 *   - the handshake: the two browsers exchange their WebRTC offer, answer and
 *     ICE candidates through here, then talk to each other directly.
 *
 * **No game traffic passes through this server.** Once the data channel is
 * open the sprite lists, joystick words and menu packets go browser to
 * browser. The server never sees an .ipa, a game file or a packet.
 *
 * Protocol: JSON text frames over one WebSocket, `t` naming the message.
 *
 *   client -> server
 *     hello   {v, build, name, room}  first message; `room` partitions the lobby
 *     lobby   {on}                    show / hide this client in the lobby
 *     invite  {to}                    ask a lobby peer for a match
 *     answer  {to, accept}            reply to an invitation
 *     signal  {to, data}              offer / answer / candidate, paired peers only
 *     leave   {}                      the match is over; unpair
 *
 *   server -> client
 *     welcome {id, ice}               your id, and the ICE servers to use
 *     peers   {list: [{id, name}]}    the lobby, without you, on every change
 *     invited {from, name}            someone wants to play
 *     answered{from, accept}          the reply to your invitation
 *     paired  {peer, name, host}      you are matched; `host` runs the fight
 *     signal  {from, data}            relayed from your partner
 *     left    {peer}                  your partner went away
 *     error   {msg}
 *
 * `host` is a fixed rule so both sides agree without another round trip: the
 * player who ACCEPTED hosts. What hosting means to the game -- which side
 * runs the fight -- is runtime/mp_session.c's business, not the lobby's.
 *
 * Environment:
 *   UMK3_SIGNAL_PORT   port on 127.0.0.1 (default 8830); nginx terminates TLS
 *   UMK3_STUN          comma-separated STUN URLs (default Cloudflare + Google)
 *   UMK3_TURN_URLS     comma-separated TURN URLs, optional
 *   UMK3_TURN_SECRET   coturn `static-auth-secret`, for time-limited TURN
 *                      credentials (the coturn REST scheme); optional
 */

'use strict';

const http = require('node:http');
const crypto = require('node:crypto');
const { WebSocketServer } = require('ws');

const PROTOCOL = 1;
const PORT = Number(process.env.UMK3_SIGNAL_PORT || 8830);
const MAX_CLIENTS = 500;
const MAX_FRAME = 16 * 1024;          // an SDP offer is a few KB
const MAX_NAME = 12;                  // the lobby row is 240 points wide
const MAX_ROOM = 24;
const MAX_LOBBY_LIST = 64;            // the game pages through six a screen
const RATE_PER_SEC = 40;              // ICE trickles in bursts; this is plenty
const RATE_BURST = 120;
const HELLO_TIMEOUT_MS = 10_000;
const PING_INTERVAL_MS = 25_000;
const TURN_TTL_S = 6 * 3600;

const STUN = (process.env.UMK3_STUN ||
              'stun:stun.cloudflare.com:3478,stun:stun.l.google.com:19302')
    .split(',').map(s => s.trim()).filter(Boolean);
const TURN_URLS = (process.env.UMK3_TURN_URLS || '')
    .split(',').map(s => s.trim()).filter(Boolean);
const TURN_SECRET = process.env.UMK3_TURN_SECRET || '';

/* id -> client. A client is { ws, id, name, room, build, inLobby, partner,
 * pendingFrom: Set<id>, tokens, last, alive }. */
const clients = new Map();

function iceServers() {
    const list = [{ urls: STUN }];
    if (TURN_URLS.length && TURN_SECRET) {
        /* coturn's REST credentials: username = expiry:label, password =
         * base64(HMAC-SHA1(secret, username)). Nothing is stored. */
        const username = `${Math.floor(Date.now() / 1000) + TURN_TTL_S}:umk3`;
        const credential = crypto.createHmac('sha1', TURN_SECRET)
            .update(username).digest('base64');
        list.push({ urls: TURN_URLS, username, credential });
    }
    return list;
}

function send(c, msg) {
    if (c.ws.readyState === c.ws.OPEN)
        c.ws.send(JSON.stringify(msg));
}

/* The lobby as one client sees it: the same room and build, visible, not in a
 * match, and not itself. */
function lobbyFor(c) {
    const list = [];
    for (const o of clients.values()) {
        if (o === c || !o.inLobby || o.partner || o.room !== c.room || o.build !== c.build)
            continue;
        list.push({ id: o.id, name: o.name });
        if (list.length >= MAX_LOBBY_LIST)
            break;
    }
    return list;
}

function broadcastLobby(room, build) {
    for (const c of clients.values())
        if (c.inLobby && !c.partner && c.room === room && c.build === build)
            send(c, { t: 'peers', list: lobbyFor(c) });
}

function cleanName(s) {
    /* The game's lobby font draws ASCII; anything else becomes a gap. */
    const n = String(s || '').toUpperCase().replace(/[^A-Z0-9 _.-]/g, '').trim();
    return n.slice(0, MAX_NAME) || 'PLAYER';
}

function cleanRoom(s) {
    return String(s || 'public').toLowerCase().replace(/[^a-z0-9_-]/g, '').slice(0, MAX_ROOM)
        || 'public';
}

function unpair(c, notify) {
    const p = c.partner ? clients.get(c.partner) : null;
    c.partner = null;
    if (p && p.partner === c.id) {
        p.partner = null;
        if (notify)
            send(p, { t: 'left', peer: c.id });
        broadcastLobby(p.room, p.build);
    }
}

function drop(c) {
    if (!clients.has(c.id))
        return;
    clients.delete(c.id);
    unpair(c, true);
    for (const o of clients.values())
        o.pendingFrom.delete(c.id);
    broadcastLobby(c.room, c.build);
}

/* A token bucket per connection: a misbehaving page is cut off, not queued. */
function allow(c) {
    const now = Date.now();
    c.tokens = Math.min(RATE_BURST, c.tokens + (now - c.last) * RATE_PER_SEC / 1000);
    c.last = now;
    if (c.tokens < 1)
        return false;
    c.tokens -= 1;
    return true;
}

function onMessage(c, raw) {
    let m;
    try {
        m = JSON.parse(raw);
    } catch {
        return send(c, { t: 'error', msg: 'bad json' });
    }
    if (!m || typeof m.t !== 'string')
        return;

    if (!c.helloDone) {
        if (m.t !== 'hello')
            return c.ws.close(1008, 'hello first');
        if (m.v !== PROTOCOL) {
            send(c, { t: 'error', msg: 'protocol', want: PROTOCOL });
            return c.ws.close(1008, 'protocol');
        }
        c.helloDone = true;
        c.name = cleanName(m.name);
        c.room = cleanRoom(m.room);
        /* Two pages only play each other when they run the same port build:
         * the sprite list is this port's memory layout, not a fixed format. */
        c.build = String(m.build || '').slice(0, 64);
        clearTimeout(c.helloTimer);
        send(c, { t: 'welcome', id: c.id, ice: iceServers() });
        return;
    }

    const to = typeof m.to === 'string' ? clients.get(m.to) : null;

    switch (m.t) {
    case 'lobby':
        c.inLobby = !!m.on;
        if (c.inLobby && !c.partner)
            send(c, { t: 'peers', list: lobbyFor(c) });
        broadcastLobby(c.room, c.build);
        break;

    case 'invite':
        if (!to || to === c || c.partner || to.partner || !to.inLobby
            || to.room !== c.room || to.build !== c.build)
            return send(c, { t: 'answered', from: m.to, accept: false });
        to.pendingFrom.add(c.id);
        send(to, { t: 'invited', from: c.id, name: c.name });
        break;

    case 'answer':
        if (!to || !c.pendingFrom.has(to.id))
            return;
        c.pendingFrom.delete(to.id);
        if (!m.accept || c.partner || to.partner) {
            send(to, { t: 'answered', from: c.id, accept: false });
            return;
        }
        /* Paired. The one who accepted hosts; both leave the lobby list. */
        c.partner = to.id;
        to.partner = c.id;
        c.pendingFrom.clear();
        send(to, { t: 'answered', from: c.id, accept: true });
        send(c,  { t: 'paired', peer: to.id, name: to.name, host: true });
        send(to, { t: 'paired', peer: c.id, name: c.name, host: false });
        broadcastLobby(c.room, c.build);
        break;

    case 'signal':
        if (!to || c.partner !== to.id || to.partner !== c.id)
            return;
        send(to, { t: 'signal', from: c.id, data: m.data });
        break;

    case 'leave':
        unpair(c, true);
        broadcastLobby(c.room, c.build);
        break;

    default:
        send(c, { t: 'error', msg: 'unknown ' + m.t.slice(0, 16) });
    }
}

const server = http.createServer((req, res) => {
    if (req.url === '/signal/health') {
        res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
        res.end(JSON.stringify({ ok: true, clients: clients.size, protocol: PROTOCOL }));
        return;
    }
    res.writeHead(404);
    res.end();
});

const wss = new WebSocketServer({ server, path: '/signal', maxPayload: MAX_FRAME });

wss.on('connection', ws => {
    if (clients.size >= MAX_CLIENTS) {
        ws.close(1013, 'full');
        return;
    }
    const c = {
        ws, id: crypto.randomBytes(6).toString('hex'), name: 'PLAYER', room: 'public',
        build: '', inLobby: false, partner: null, pendingFrom: new Set(),
        tokens: RATE_BURST, last: Date.now(), alive: true, helloDone: false,
    };
    c.helloTimer = setTimeout(() => ws.close(1008, 'no hello'), HELLO_TIMEOUT_MS);
    clients.set(c.id, c);

    ws.on('pong', () => { c.alive = true; });
    ws.on('message', (data, isBinary) => {
        if (isBinary || !allow(c)) {
            ws.close(1008, isBinary ? 'text only' : 'rate');
            return;
        }
        onMessage(c, data.toString('utf8'));
    });
    ws.on('close', () => {
        clearTimeout(c.helloTimer);
        drop(c);
    });
    ws.on('error', () => {});
});

/* Dead connections (a closed laptop lid) leave the lobby within a minute. */
setInterval(() => {
    for (const c of clients.values()) {
        if (!c.alive) {
            c.ws.terminate();
            continue;
        }
        c.alive = false;
        c.ws.ping();
    }
}, PING_INTERVAL_MS);

server.listen(PORT, '127.0.0.1', () => {
    console.log(`umk3 signal: protocol ${PROTOCOL}, ws://127.0.0.1:${PORT}/signal`);
});
