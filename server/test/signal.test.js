'use strict';
/* Two clients through the whole lobby: hello, lobby, invite, answer, paired,
 * signal relay, and the partner leaving. Runs the real server as a child. */

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const net = require('node:net');

function freePort() {
    return new Promise(resolve => {
        const s = net.createServer().listen(0, '127.0.0.1', () => {
            const { port } = s.address();
            s.close(() => resolve(port));
        });
    });
}

function client(url) {
    const ws = new WebSocket(url);
    const inbox = [];
    const waiters = [];
    ws.onmessage = ev => {
        const m = JSON.parse(ev.data);
        const i = waiters.findIndex(w => w.t === m.t);
        if (i >= 0) waiters.splice(i, 1)[0].resolve(m);
        else inbox.push(m);
    };
    return {
        ws,
        open: () => new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; }),
        send: m => ws.send(JSON.stringify(m)),
        next: t => {
            const i = inbox.findIndex(m => m.t === t);
            if (i >= 0) return Promise.resolve(inbox.splice(i, 1)[0]);
            return new Promise(resolve => waiters.push({ t, resolve }));
        },
        drain: t => { for (let i = inbox.length - 1; i >= 0; i--) if (inbox[i].t === t) inbox.splice(i, 1); },
        close: () => ws.close(),
    };
}

test('lobby, invitation, pairing, relay and leave', async () => {
    const port = await freePort();
    const srv = spawn(process.execPath, [path.join(__dirname, '..', 'signal.js')],
                      { env: { ...process.env, UMK3_SIGNAL_PORT: String(port) }, stdio: 'pipe' });
    try {
        await new Promise(r => srv.stdout.once('data', r));
        const url = `ws://127.0.0.1:${port}/signal`;
        const a = client(url), b = client(url), other = client(url);
        await Promise.all([a.open(), b.open(), other.open()]);

        a.send({ t: 'hello', v: 1, build: 'x1', name: 'kitana!', room: 'Test' });
        b.send({ t: 'hello', v: 1, build: 'x1', name: 'Kung Lao', room: 'test' });
        other.send({ t: 'hello', v: 1, build: 'x2', name: 'other', room: 'test' });
        const wa = await a.next('welcome'), wb = await b.next('welcome');
        await other.next('welcome');
        assert.ok(wa.ice.length >= 1 && wa.ice[0].urls.length >= 1);

        a.send({ t: 'lobby', on: true });
        await a.next('peers');
        b.send({ t: 'lobby', on: true });
        other.send({ t: 'lobby', on: true });
        const pb = await b.next('peers');
        assert.deepEqual(pb.list, [{ id: wa.id, name: 'KITANA' }]);   // other build hidden
        let pa;
        do pa = await a.next('peers'); while (pa.list.length === 0);
        assert.deepEqual(pa.list, [{ id: wb.id, name: 'KUNG LAO' }]);

        a.send({ t: 'invite', to: wb.id });
        const inv = await b.next('invited');
        assert.equal(inv.from, wa.id);
        b.send({ t: 'answer', to: wa.id, accept: true });
        const ans = await a.next('answered');
        assert.equal(ans.accept, true);
        const pairA = await a.next('paired'), pairB = await b.next('paired');
        assert.equal(pairB.host, true);                                 // the one who accepted
        assert.equal(pairA.host, false);
        assert.equal(pairA.peer, wb.id);

        a.send({ t: 'signal', to: wb.id, data: { sdp: 'offer' } });
        const sig = await b.next('signal');
        assert.deepEqual(sig, { t: 'signal', from: wa.id, data: { sdp: 'offer' } });

        other.send({ t: 'signal', to: wb.id, data: 'nope' });          // not paired: dropped
        a.close();
        const left = await b.next('left');
        assert.equal(left.peer, wa.id);
        b.close();
        other.close();
    } finally {
        srv.kill();
    }
});

test('hello is required first and the protocol is checked', async () => {
    const port = await freePort();
    const srv = spawn(process.execPath, [path.join(__dirname, '..', 'signal.js')],
                      { env: { ...process.env, UMK3_SIGNAL_PORT: String(port) }, stdio: 'pipe' });
    try {
        await new Promise(r => srv.stdout.once('data', r));
        const url = `ws://127.0.0.1:${port}/signal`;
        const c = client(url);
        await c.open();
        const closed = new Promise(r => { c.ws.onclose = ev => r(ev.code); });
        c.send({ t: 'hello', v: 99, build: 'x', name: 'a' });
        const err = await c.next('error');
        assert.equal(err.msg, 'protocol');
        assert.equal(await closed, 1008);
    } finally {
        srv.kill();
    }
});
