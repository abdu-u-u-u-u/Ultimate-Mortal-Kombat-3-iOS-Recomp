/*
 * mp_webrtc.js -- the browser's radio: the transport under runtime/mp_session.c.
 *
 * An Emscripten JS library (linked with --js-library). It implements the small
 * C interface in runtime/mp_net.h on top of two browser facilities:
 *
 *   a WebSocket to the lobby service (server/signal.js), which plays the part
 *   GKSession's Bluetooth advertising played on the iPhone: who is in the
 *   multiplayer screen, who wants to play whom;
 *
 *   one WebRTC data channel to the opponent, which carries every game packet,
 *   browser to browser. It is RELIABLE and ORDERED: the host's per-tick sprite
 *   list carries the fight's event queue (health, sounds, finishers), and a
 *   dropped one would lose those events for good. At 56 ticks a second and a
 *   few hundred bytes a tick, SCTP's retransmission costs a little latency on
 *   a lossy link and nothing otherwise.
 *
 * The C side polls. Nothing here calls into C: incoming messages and lobby
 * changes are queued, and mp_session.c reads them once a frame from
 * heartbeatUpdate. That keeps every write to game globals on the game's own
 * thread of control, between frames, the way the iPhone's run loop delivered
 * GKSession callbacks between frames.
 *
 * Page configuration (all optional), read when the lobby opens:
 *   Module.umk3SignalUrl   wss://host/signal; default: this page's origin
 *   Module.umk3PlayerName  the name other players see; default from the page
 *   Module.umk3Room        lobby partition; default "public" (?room= in the URL)
 */

addToLibrary({
  $UMK3NET__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  /* The page reads the link's state for its status line (web/game.html). */
  $UMK3NET__postset: 'Module["umk3Net"] = UMK3NET;',
  $UMK3NET: {
    /* states, as runtime/mp_net.h numbers them */
    OFF: 0, LOBBY: 1, CONNECTING: 2, CONNECTED: 3, CLOSED: 4,

    state: 0,
    ws: null,
    wsReady: false,
    myId: null,
    ice: [],
    wantLobby: false,
    peers: [],              /* [{id, name}] as the server last sent them */
    invitation: null,       /* {from, name} waiting for mpnet_answer */
    inviteSent: null,       /* the id we invited, until answered */
    partner: null,          /* {id, name, host} once paired */
    pc: null,
    dc: null,
    inbox: [],              /* Uint8Array messages from the opponent */
    pendingIce: [],         /* candidates that arrived before the remote SDP */
    build: 0,
    log: function(msg) { if (Module.umk3NetLog) Module.umk3NetLog(msg); else console.log('[mp] ' + msg); },

    signalUrl: function() {
      if (Module.umk3SignalUrl) return Module.umk3SignalUrl;
      var q = new URLSearchParams(location.search).get('signal');
      if (q) return q;
      return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/signal';
    },
    room: function() {
      return Module.umk3Room || new URLSearchParams(location.search).get('room') || 'public';
    },
    name: function() {
      if (Module.umk3PlayerName) return Module.umk3PlayerName;
      try { var n = localStorage.getItem('umk3.name'); if (n) return n; } catch (e) {}
      return 'PLAYER' + Math.floor(1000 + Math.random() * 9000);
    },

    send: function(msg) {
      if (UMK3NET.ws && UMK3NET.wsReady) UMK3NET.ws.send(JSON.stringify(msg));
    },

    open: function() {
      if (UMK3NET.ws) return;
      var ws;
      try {
        ws = new WebSocket(UMK3NET.signalUrl());
      } catch (e) {
        UMK3NET.log('lobby: ' + e.message);
        UMK3NET.state = UMK3NET.CLOSED;
        return;
      }
      UMK3NET.ws = ws;
      UMK3NET.wsReady = false;
      ws.onopen = function() {
        UMK3NET.wsReady = true;
        UMK3NET.send({ t: 'hello', v: 1, build: String(UMK3NET.build),
                       name: UMK3NET.name(), room: UMK3NET.room() });
      };
      ws.onmessage = function(ev) {
        var m;
        try { m = JSON.parse(ev.data); } catch (e) { return; }
        UMK3NET.onSignal(m);
      };
      ws.onclose = function() {
        UMK3NET.ws = null;
        UMK3NET.wsReady = false;
        UMK3NET.peers = [];
        /* A match in progress keeps its data channel; only the lobby is gone. */
        if (UMK3NET.state === UMK3NET.LOBBY || UMK3NET.state === UMK3NET.CONNECTING)
          UMK3NET.state = UMK3NET.CLOSED;
      };
      ws.onerror = function() {};
    },

    onSignal: function(m) {
      switch (m.t) {
      case 'welcome':
        UMK3NET.myId = m.id;
        UMK3NET.ice = m.ice || [];
        if (UMK3NET.wantLobby) UMK3NET.send({ t: 'lobby', on: true });
        break;
      case 'peers':
        UMK3NET.peers = m.list || [];
        break;
      case 'invited':
        /* One invitation at a time; a second while one is open is declined. */
        if (UMK3NET.invitation || UMK3NET.partner || UMK3NET.inviteSent)
          UMK3NET.send({ t: 'answer', to: m.from, accept: false });
        else
          UMK3NET.invitation = { from: m.from, name: m.name };
        break;
      case 'answered':
        if (UMK3NET.inviteSent === m.from && !m.accept) {
          UMK3NET.inviteSent = null;
          if (UMK3NET.state === UMK3NET.CONNECTING) UMK3NET.state = UMK3NET.LOBBY;
        }
        break;
      case 'paired':
        UMK3NET.inviteSent = null;
        UMK3NET.invitation = null;
        UMK3NET.partner = { id: m.peer, name: m.name, host: !!m.host };
        UMK3NET.state = UMK3NET.CONNECTING;
        UMK3NET.startRtc();
        break;
      case 'signal':
        if (UMK3NET.partner && m.from === UMK3NET.partner.id) UMK3NET.onRtcSignal(m.data);
        break;
      case 'left':
        if (UMK3NET.partner && m.peer === UMK3NET.partner.id) UMK3NET.closeRtc('partner left');
        break;
      case 'error':
        UMK3NET.log('lobby error: ' + m.msg);
        break;
      }
    },

    /* The guest offers, the host answers. The lobby makes the player who
     * accepted the invitation the host (server/signal.js). */
    startRtc: function() {
      var pc = new RTCPeerConnection({ iceServers: UMK3NET.ice });
      UMK3NET.pc = pc;
      UMK3NET.pendingIce = [];
      pc.onicecandidate = function(ev) {
        if (ev.candidate) UMK3NET.send({ t: 'signal', to: UMK3NET.partner.id,
                                          data: { candidate: ev.candidate.toJSON() } });
      };
      pc.onconnectionstatechange = function() {
        if (pc.connectionState === 'failed' || pc.connectionState === 'closed')
          UMK3NET.closeRtc('connection ' + pc.connectionState);
      };
      if (UMK3NET.partner.host) {
        pc.ondatachannel = function(ev) { UMK3NET.useChannel(ev.channel); };
      } else {
        UMK3NET.useChannel(pc.createDataChannel('umk3', { ordered: true }));
        pc.createOffer().then(function(o) { return pc.setLocalDescription(o); })
          .then(function() {
            UMK3NET.send({ t: 'signal', to: UMK3NET.partner.id,
                           data: { sdp: pc.localDescription.toJSON() } });
          })
          .catch(function(e) { UMK3NET.closeRtc('offer: ' + e.message); });
      }
    },

    onRtcSignal: function(d) {
      var pc = UMK3NET.pc;
      if (!pc || !d) return;
      if (d.sdp) {
        pc.setRemoteDescription(d.sdp).then(function() {
          var queued = UMK3NET.pendingIce;
          UMK3NET.pendingIce = [];
          queued.forEach(function(c) { pc.addIceCandidate(c).catch(function() {}); });
          if (d.sdp.type === 'offer')
            return pc.createAnswer().then(function(a) { return pc.setLocalDescription(a); })
              .then(function() {
                UMK3NET.send({ t: 'signal', to: UMK3NET.partner.id,
                               data: { sdp: pc.localDescription.toJSON() } });
              });
        }).catch(function(e) { UMK3NET.closeRtc('sdp: ' + e.message); });
      } else if (d.candidate) {
        if (pc.remoteDescription) pc.addIceCandidate(d.candidate).catch(function() {});
        else UMK3NET.pendingIce.push(d.candidate);
      }
    },

    useChannel: function(dc) {
      UMK3NET.dc = dc;
      dc.binaryType = 'arraybuffer';
      dc.onopen = function() {
        UMK3NET.state = UMK3NET.CONNECTED;
        UMK3NET.log('connected to ' + UMK3NET.partner.name +
                    (UMK3NET.partner.host ? ' (you host)' : ' (they host)'));
      };
      dc.onmessage = function(ev) {
        if (ev.data instanceof ArrayBuffer) UMK3NET.inbox.push(new Uint8Array(ev.data));
      };
      dc.onclose = function() { UMK3NET.closeRtc('channel closed'); };
    },

    closeRtc: function(why) {
      if (UMK3NET.state === UMK3NET.CLOSED && !UMK3NET.pc) return;
      UMK3NET.log('match ended: ' + why);
      if (UMK3NET.dc) { try { UMK3NET.dc.close(); } catch (e) {} }
      if (UMK3NET.pc) { try { UMK3NET.pc.close(); } catch (e) {} }
      UMK3NET.dc = null;
      UMK3NET.pc = null;
      if (UMK3NET.partner) UMK3NET.send({ t: 'leave' });
      UMK3NET.partner = null;
      UMK3NET.state = UMK3NET.CLOSED;
    },
  },

  mpnet_start__deps: ['$UMK3NET'],
  mpnet_start: function(build) {
    UMK3NET.build = build >>> 0;
    UMK3NET.wantLobby = true;
    UMK3NET.inbox = [];
    if (UMK3NET.state === UMK3NET.OFF || UMK3NET.state === UMK3NET.CLOSED)
      UMK3NET.state = UMK3NET.LOBBY;
    UMK3NET.open();
    UMK3NET.send({ t: 'lobby', on: true });
  },

  mpnet_stop__deps: ['$UMK3NET'],
  mpnet_stop: function() {
    UMK3NET.wantLobby = false;
    UMK3NET.closeRtc('left multiplayer');
    UMK3NET.send({ t: 'lobby', on: false });
    if (UMK3NET.ws) { try { UMK3NET.ws.close(); } catch (e) {} }
    UMK3NET.ws = null;
    UMK3NET.peers = [];
    UMK3NET.invitation = null;
    UMK3NET.inviteSent = null;
    UMK3NET.inbox = [];
    UMK3NET.state = UMK3NET.OFF;
  },

  /* After a disconnect the game goes back to its lobby: show this player again. */
  mpnet_relist__deps: ['$UMK3NET'],
  mpnet_relist: function() {
    UMK3NET.closeRtc('relist');
    UMK3NET.inbox = [];
    UMK3NET.state = UMK3NET.LOBBY;
    if (!UMK3NET.ws) UMK3NET.open();
    else UMK3NET.send({ t: 'lobby', on: true });
  },

  mpnet_state__deps: ['$UMK3NET'],
  mpnet_state: function() { return UMK3NET.state; },

  mpnet_peer_count__deps: ['$UMK3NET'],
  mpnet_peer_count: function() { return UMK3NET.peers.length; },

  mpnet_peer_name__deps: ['$UMK3NET'],
  mpnet_peer_name: function(i, out, n) {
    var p = UMK3NET.peers[i];
    stringToUTF8(p ? p.name : '', out, n);
    return p ? 1 : 0;
  },

  mpnet_invite__deps: ['$UMK3NET'],
  mpnet_invite: function(i) {
    var p = UMK3NET.peers[i];
    if (!p || UMK3NET.partner || UMK3NET.inviteSent) return 0;
    UMK3NET.inviteSent = p.id;
    UMK3NET.state = UMK3NET.CONNECTING;
    UMK3NET.send({ t: 'invite', to: p.id });
    return 1;
  },

  mpnet_invitation__deps: ['$UMK3NET'],
  mpnet_invitation: function(out, n) {
    if (!UMK3NET.invitation) return 0;
    stringToUTF8(UMK3NET.invitation.name, out, n);
    return 1;
  },

  mpnet_answer__deps: ['$UMK3NET'],
  mpnet_answer: function(accept) {
    var inv = UMK3NET.invitation;
    if (!inv) return;
    UMK3NET.invitation = null;
    UMK3NET.send({ t: 'answer', to: inv.from, accept: !!accept });
    if (accept) UMK3NET.state = UMK3NET.CONNECTING;
  },

  mpnet_is_host__deps: ['$UMK3NET'],
  mpnet_is_host: function() { return UMK3NET.partner && UMK3NET.partner.host ? 1 : 0; },

  mpnet_partner_name__deps: ['$UMK3NET'],
  mpnet_partner_name: function(out, n) {
    stringToUTF8(UMK3NET.partner ? UMK3NET.partner.name : '', out, n);
  },

  /* Reliable and ordered; returns 0 when there is no open channel. The
   * channel's own buffer absorbs bursts; bufferedAmount past 1 MB means the
   * other side has stopped reading, and the match is ended rather than letting
   * the page's memory grow without bound. */
  mpnet_send__deps: ['$UMK3NET'],
  mpnet_send: function(ptr, len) {
    var dc = UMK3NET.dc;
    if (!dc || dc.readyState !== 'open') return 0;
    if (dc.bufferedAmount > (1 << 20)) {
      UMK3NET.closeRtc('peer not reading');
      return 0;
    }
    dc.send(HEAPU8.slice(ptr, ptr + len));
    return 1;
  },

  /* The next message into `ptr` (at most `cap` bytes): its length, 0 when the
   * inbox is empty, or -1 for a message that does not fit (dropped). */
  mpnet_recv__deps: ['$UMK3NET'],
  mpnet_recv: function(ptr, cap) {
    var m = UMK3NET.inbox.shift();
    if (!m) return 0;
    if (m.length > cap) return -1;
    HEAPU8.set(m, ptr);
    return m.length;
  },
});
