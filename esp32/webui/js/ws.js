/*
 * ws.js — WebSocket MIDI transport for the web keyboard.
 *
 * Sends Note On/Off/CC as small JSON frames over ONE socket instead of an HTTP
 * request per event (correction #28). NoteOff and panic jump the outgoing queue
 * so a release is never starved behind a burst of NoteOn/CC (correction #27),
 * and pending frames are flushed (priority-first) on reconnect.
 */

export class MidiSocket {
  // socketFactory(url) -> a WebSocket-like object (injectable for tests).
  constructor(url, { socketFactory = (u) => new WebSocket(u), token = "", maxQueue = 64 } = {}) {
    this.url = url;
    this.socketFactory = socketFactory;
    this.token = token;
    this.maxQueue = maxQueue;
    this.queue = [];
    this.ws = null;
    this.open = false;
    this.backoff = 500;
    this._timer = null;
    this._intentional = false;
    this.onStatus = () => {};
    this.onMessage = () => {};   // server replies (e.g. auth/command errors)
  }

  connect() {
    if (this._timer) { clearTimeout(this._timer); this._timer = null; }   // no double reconnect (#42)
    this._intentional = false;
    this.ws = this.socketFactory(this.url);
    this.ws.onopen = () => {
      this.open = true; this.backoff = 500; this.onStatus("open");
      if (this.token) this._raw({ auth: this.token });   // authenticate this socket first
      this.flush();
    };
    this.ws.onclose = () => {
      this.open = false; this.onStatus("closed");
      if (!this._intentional) this._reconnectLater();     // don't reconnect an intentional close (#42)
    };
    this.ws.onerror = () => { try { this.ws.close(); } catch { /* ignore */ } };
    this.ws.onmessage = (ev) => {
      let msg = null; try { msg = JSON.parse(ev.data); } catch { /* ignore */ }
      if (msg) this.onMessage(msg);
    };
    return this;
  }

  _reconnectLater() {
    if (this._timer) return;
    const wait = this.backoff;
    this.backoff = Math.min(this.backoff * 2, 8000);
    this._timer = setTimeout(() => { this._timer = null; this.connect(); }, wait);
  }

  _priority(cmd) { return cmd.type === "noteOff" || cmd.type === "panic"; }

  send(cmd) {
    const frame = this.token ? { ...cmd, token: this.token } : cmd;
    if (this.open) { this._raw(frame); return true; }
    this._enqueue(frame);
    return false;
  }

  // Bounded outgoing queue (#43): priority frames to the front; coalesce CC on
  // the same channel/controller; drop oldest non-priority frames when full so a
  // long disconnect can't accumulate unbounded stale events.
  _enqueue(frame) {
    if (frame.type === "cc") {
      const i = this.queue.findIndex((q) => q.type === "cc" && q.channel === frame.channel && q.a === frame.a);
      if (i >= 0) { this.queue[i] = frame; return; }   // coalesce
    }
    if (this._priority(frame)) this.queue.unshift(frame);
    else this.queue.push(frame);
    while (this.queue.length > this.maxQueue) {
      // drop the oldest NON-priority frame; never drop a NoteOff/panic
      const idx = this.queue.findIndex((q) => !this._priority(q));
      this.queue.splice(idx >= 0 ? idx : 0, 1);
    }
  }

  flush() {
    // send priority frames first, preserving order within each class
    const prio = this.queue.filter((c) => this._priority(c));
    const rest = this.queue.filter((c) => !this._priority(c));
    this.queue = [];
    for (const c of prio) this._raw(c);
    for (const c of rest) this._raw(c);
  }

  _raw(frame) { this.ws.send(JSON.stringify(frame)); }

  close() {
    this._intentional = true;
    if (this._timer) { clearTimeout(this._timer); this._timer = null; }
    if (this.ws) this.ws.close();
  }
}
