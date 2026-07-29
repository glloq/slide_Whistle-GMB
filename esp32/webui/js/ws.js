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
  constructor(url, { socketFactory = (u) => new WebSocket(u), token = "" } = {}) {
    this.url = url;
    this.socketFactory = socketFactory;
    this.token = token;
    this.queue = [];
    this.ws = null;
    this.open = false;
    this.backoff = 500;
    this.onStatus = () => {};
  }

  connect() {
    this.ws = this.socketFactory(this.url);
    this.ws.onopen = () => {
      this.open = true; this.backoff = 500; this.onStatus("open");
      this.flush();
    };
    this.ws.onclose = () => {
      this.open = false; this.onStatus("closed");
      this._reconnectLater();
    };
    this.ws.onerror = () => { try { this.ws.close(); } catch { /* ignore */ } };
    return this;
  }

  _reconnectLater() {
    const wait = this.backoff;
    this.backoff = Math.min(this.backoff * 2, 8000);
    this._timer = setTimeout(() => this.connect(), wait);
  }

  _priority(cmd) { return cmd.type === "noteOff" || cmd.type === "panic"; }

  send(cmd) {
    const frame = this.token ? { ...cmd, token: this.token } : cmd;
    if (this.open) { this._raw(frame); return true; }
    // not connected: queue, priority to the front
    if (this._priority(frame)) this.queue.unshift(frame);
    else this.queue.push(frame);
    return false;
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

  close() { if (this._timer) clearTimeout(this._timer); if (this.ws) this.ws.close(); }
}
