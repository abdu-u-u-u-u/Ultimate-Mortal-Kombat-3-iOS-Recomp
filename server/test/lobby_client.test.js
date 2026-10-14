'use strict';
/* web/mp_webrtc.js's lobby logic against the real server/signal.js, in Node:
 * the library is loaded as written, each "browser" in its own vm context,
 * with RTCPeerConnection stubbed out (pairing is the subject, not WebRTC;
 * tests/web_mp_link.c covers the data channel in real browsers). */

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const vm = require('node:vm');

const LIB = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'mp_webrtc.js'), 'utf8');

function freePort() {
    return new Promise(resolve => {
        const s = net.createServer().listen(0, '127.0.0.1', () => {
            const { port } = s.address();
            s.close(() => resolve(port));
        });
    });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function until(what, cond, ms = 3000) {
    const end = Date.now() + ms;
    while (!cond()) {
        if (Date.now() > end) throw new Error('timed out waiting for ' + what);
        await sleep(10);
    }
}

/* One page: the library's functions bound to their own UMK3NET. */
function page(url, name) {
    const rtc = [];
    class FakePeerConnection {
        constructor(cfg) { this.cfg = cfg; rtc.push(this); this.remoteDescription = null; }
        createDataChannel() { return {}; }
        createOffer() { return new Promise(() => {}); }   // the handshake is not the subject
        setRemoteDescription() { return new Promise(() => {}); }
        addIceCandidate() { return Promise.resolve(); }
        close() {}
    }
    const ctx = {
        Module: { umk3SignalUrl: url, umk3PlayerName: name, umk3Room: 'test', umk3NetLog() {} },
        location: { protocol: 'http:', host: 'test', search: '' },
        URLSearchParams, WebSocket, JSON, Math, console, setTimeout, clearTimeout,
        RTCPeerConnection: FakePeerConnection,
        HEAPU8: new Uint8Array(256),
        stringToUTF8(s, ptr, n) { ctx.lastString = s; },
        UTF8ToString() { return ''; },
        lengthBytesUTF8(s) { return s.length; },
        addToLibrary(lib) { ctx.lib = lib; },
    };
    vm.createContext(ctx);
    vm.runInContext(LIB, ctx);
    ctx.UMK3NET = ctx.lib.$UMK3NET;
    const call = (fn, ...args) => ctx.lib[fn](...args);
    return { ctx, net: ctx.UMK3NET, call, rtc };
}

async function startServer(port) {
    const srv = spawn(process.execPath, [path.join(__dirname, '..', 'signal.js')],
                      { env: { ...process.env, UMK3_SIGNAL_PORT: String(port) }, stdio: 'pipe' });
    await new Promise(r => srv.stdout.once('data', r));
    srv.stdout.resume();                // its event log must not fill the pipe
    return srv;
}

async function withServer(fn) {
    const port = await freePort();
    const srv = await startServer(port);
    try {
        await fn(`ws://127.0.0.1:${port}/signal`);
    } finally {
        srv.kill();
    }
}

async function lobbyPair(url) {
    const a = page(url, 'ALPHA'), b = page(url, 'BRAVO');
    a.call('mpnet_start', 7);
    b.call('mpnet_start', 7);
    await until('both to see each other',
                () => a.net.peers.length === 1 && b.net.peers.length === 1);
    return [a, b];
}

function close(...pages) {
    for (const p of pages) p.call('mpnet_stop');
}

test('an invitation accepted pairs the two, the accepter hosting', async () => {
    await withServer(async url => {
        const [a, b] = await lobbyPair(url);
        assert.equal(a.call('mpnet_invite', 0), 1);
        await until('the invitation', () => b.net.invitation !== null);
        assert.equal(b.call('mpnet_invitation', 0, 64), 1);
        assert.equal(b.ctx.lastString, 'ALPHA');
        b.call('mpnet_answer', 1);
        await until('pairing', () => a.net.partner && b.net.partner);
        assert.equal(b.net.partner.host, true);
        assert.equal(a.net.partner.host, false);
        assert.equal(a.rtc.length, 1);
        assert.equal(b.rtc.length, 1);
        close(a, b);
    });
});

test('a declined invitation leaves both in the lobby', async () => {
    await withServer(async url => {
        const [a, b] = await lobbyPair(url);
        a.call('mpnet_invite', 0);
        await until('the invitation', () => b.net.invitation !== null);
        b.call('mpnet_answer', 0);
        await until('the answer', () => a.net.inviteSent === null);
        assert.equal(a.call('mpnet_state'), 1);      // LOBBY again
        assert.equal(a.net.partner, null);
        close(a, b);
    });
});

test('two players who invite each other at once are paired once', async () => {
    await withServer(async url => {
        const [a, b] = await lobbyPair(url);
        assert.equal(a.call('mpnet_invite', 0), 1);  // crossed on the wire
        assert.equal(b.call('mpnet_invite', 0), 1);
        await until('pairing', () => a.net.partner && b.net.partner);
        assert.notEqual(a.net.partner.host, b.net.partner.host);
        const lower = a.net.myId < b.net.myId ? a : b;
        assert.equal(lower.net.partner.host, true);  // the lower id accepted
        assert.equal(a.rtc.length + b.rtc.length, 2);
        close(a, b);
    });
});

test('a lobby that loses the server reconnects and lists the players again', async () => {
    const port = await freePort();
    let srv = await startServer(port);
    try {
        const url = `ws://127.0.0.1:${port}/signal`;
        const [a, b] = await lobbyPair(url);
        srv.kill();                     // a server restart, a network change
        await new Promise(r => srv.once('exit', r));
        await until('both to notice', () => a.net.myId === null && b.net.myId === null);
        assert.equal(a.call('mpnet_state'), 1);      // still in the lobby: retrying
        assert.equal(a.net.peers.length, 0);
        srv = await startServer(port);
        await until('both to be listed again',
                    () => a.net.peers.length === 1 && b.net.peers.length === 1, 10000);
        assert.ok(a.net.myId && b.net.myId);
        close(a, b);
        assert.equal(a.net.retryTimer, null);
    } finally {
        srv.kill();
    }
});
