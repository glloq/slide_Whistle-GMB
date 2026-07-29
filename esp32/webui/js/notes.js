/*
 * notes.js — active-notes registry for the web keyboard.
 *
 * Tracks every note the browser has turned on and can flush them all with
 * NoteOff, so notes never get stuck when focus is lost, the tab is hidden, the
 * WebSocket drops, or the user navigates away (correction #27).
 */

export class NoteRegistry {
  // send(kind, {channel, note}) — kind is "noteOn" | "noteOff"
  constructor(send) {
    this.send = send;
    this.active = new Map(); // key "ch:note" -> {channel, note}
  }
  key(channel, note) { return `${channel}:${note}`; }

  noteOn(channel, note, velocity = 100) {
    this.active.set(this.key(channel, note), { channel, note });
    this.send("noteOn", { channel, note, velocity });
  }
  noteOff(channel, note) {
    if (this.active.delete(this.key(channel, note)))
      this.send("noteOff", { channel, note });
  }
  isActive(channel, note) { return this.active.has(this.key(channel, note)); }
  size() { return this.active.size; }

  // Send NoteOff for every active note and clear the registry.
  allOff() {
    for (const { channel, note } of this.active.values())
      this.send("noteOff", { channel, note });
    this.active.clear();
  }
}

// Wire the registry to browser lifecycle events. `win`/`doc` are injectable for
// tests; each of these must flush stuck notes.
export function bindLifecycleFlush(registry, { win = window, doc = document } = {}) {
  const flush = () => registry.allOff();
  win.addEventListener("blur", flush);
  win.addEventListener("pagehide", flush);
  doc.addEventListener("visibilitychange", () => { if (doc.hidden) flush(); });
  return flush; // returned so callers can also trigger it (e.g. on WS close)
}
