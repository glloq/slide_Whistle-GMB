/*
 * ui.test.mjs — node --test suite for the web UI logic modules.
 * Covers Section 18 "Web/API" frontend items: HTTP errors never read as
 * success (#23), macros stop on error (#24), no innerHTML (#25), diff updates
 * (#26), stuck-note flush (#27), WS MIDI priority (#28).
 */
import test from "node:test";
import assert from "node:assert/strict";

import { makeApi, ApiError } from "../../esp32/webui/js/api.js";
import { NoteRegistry, bindLifecycleFlush } from "../../esp32/webui/js/notes.js";
import { MidiSocket } from "../../esp32/webui/js/ws.js";
import { runMacro } from "../../esp32/webui/js/macros.js";
import { h, clear, diffKeys, patchText } from "../../esp32/webui/js/dom.js";
import { configNeedsRestart, UnsavedTracker, deepMerge, midiFlagsFor, applyWizardPatch } from "../../esp32/webui/js/config.js";
import { Wizard } from "../../esp32/webui/js/wizard.js";

// --- helpers ---------------------------------------------------------------
function fakeRes(status, obj) {
  return { ok: status >= 200 && status < 300, status, text: async () => JSON.stringify(obj) };
}

// --- api.js ----------------------------------------------------------------
test("api: ok response returns data", async () => {
  const api = makeApi({ fetchImpl: async () => fakeRes(200, { ok: true, data: { x: 1 } }) });
  assert.deepEqual(await api.status(), { x: 1 });
});

test("api: HTTP 400 throws ApiError with code/field (never success)", async () => {
  const api = makeApi({ fetchImpl: async () =>
    fakeRes(400, { ok: false, error: { code: "GPIO_CONFLICT", message: "dup", field: "a.b" } }) });
  await assert.rejects(() => api.putConfig({}), (e) => {
    assert.ok(e instanceof ApiError);
    assert.equal(e.status, 400);
    assert.equal(e.code, "GPIO_CONFLICT");
    assert.equal(e.field, "a.b");
    return true;
  });
});

test("api: ok:false body on a 200 still throws", async () => {
  const api = makeApi({ fetchImpl: async () => fakeRes(200, { ok: false, error: { code: "X" } }) });
  await assert.rejects(() => api.status(), ApiError);
});

test("api: sends auth token header", async () => {
  let seen = null;
  const api = makeApi({ getToken: () => "tok123", fetchImpl: async (_u, o) => { seen = o.headers; return fakeRes(200, { ok: true, data: {} }); } });
  await api.status();
  assert.equal(seen["X-Auth-Token"], "tok123");
});

// --- notes.js --------------------------------------------------------------
test("notes: registry tracks and flushes all NoteOff", () => {
  const sent = [];
  const reg = new NoteRegistry((kind, m) => sent.push([kind, m.channel, m.note]));
  reg.noteOn(1, 60); reg.noteOn(1, 64);
  assert.equal(reg.size(), 2);
  reg.noteOff(1, 60);
  assert.equal(reg.size(), 1);
  reg.allOff();                       // flush the stuck note (64)
  assert.equal(reg.size(), 0);
  // one explicit noteOff (60) + one flushed (64) = 2
  assert.equal(sent.filter((s) => s[0] === "noteOff").length, 2);
});

test("notes: lifecycle blur flushes stuck notes (#27)", () => {
  const sent = [];
  const reg = new NoteRegistry((kind) => sent.push(kind));
  const listeners = {};
  const win = { addEventListener: (e, f) => (listeners[e] = f) };
  const doc = { addEventListener: () => {}, hidden: false };
  bindLifecycleFlush(reg, { win, doc });
  reg.noteOn(1, 60);
  listeners.blur();                   // simulate window blur
  assert.equal(reg.size(), 0);
});

// --- ws.js -----------------------------------------------------------------
class FakeWS {
  constructor() { this.sent = []; this.readyState = 0; }
  send(s) { this.sent.push(s); }
  close() { this.onclose && this.onclose(); }
}
test("ws: queues while closed, flushes NoteOff first on open (#27/#28)", () => {
  let ws;
  const sock = new MidiSocket("ws://x", { socketFactory: () => (ws = new FakeWS()) });
  sock.connect();
  // not open yet: enqueue a NoteOn then a NoteOff
  sock.send({ type: "noteOn", channel: 1, note: 60, velocity: 100 });
  sock.send({ type: "noteOff", channel: 1, note: 60 });
  ws.onopen();                        // socket opens → flush
  const kinds = ws.sent.map((s) => JSON.parse(s).type);
  assert.deepEqual(kinds[0], "noteOff");   // priority frame first
  assert.equal(ws.sent.length, 2);
});

test("ws: single socket, one frame per event (not HTTP)", () => {
  let ws;
  const sock = new MidiSocket("ws://x", { socketFactory: () => (ws = new FakeWS()) });
  sock.connect(); ws.onopen();
  sock.send({ type: "noteOn", channel: 1, note: 60 });
  sock.send({ type: "cc", channel: 1, a: 11, b: 64 });
  assert.equal(ws.sent.length, 2);
});

test("ws: bounded queue drops old NoteOn but keeps NoteOff (#43)", () => {
  const sock = new MidiSocket("ws://x", { socketFactory: () => new FakeWS(), maxQueue: 4 });
  sock.connect();                       // never opened → everything queues
  for (let n = 0; n < 20; n++) sock.send({ type: "noteOn", channel: 1, note: 60 + n });
  sock.send({ type: "noteOff", channel: 1, note: 60 });
  assert.ok(sock.queue.length <= 4);
  assert.ok(sock.queue.some((q) => q.type === "noteOff"));   // release preserved
});

test("ws: coalesces CC on same channel/controller (#43)", () => {
  const sock = new MidiSocket("ws://x", { socketFactory: () => new FakeWS(), maxQueue: 32 });
  sock.connect();
  sock.send({ type: "cc", channel: 1, a: 11, b: 10 });
  sock.send({ type: "cc", channel: 1, a: 11, b: 99 });   // replaces the previous
  const ccs = sock.queue.filter((q) => q.type === "cc" && q.a === 11);
  assert.equal(ccs.length, 1);
  assert.equal(ccs[0].b, 99);
});

test("ws: reconnect discards stale queue and sends Panic (#5 §18)", () => {
  let ws;
  const sock = new MidiSocket("ws://x", { socketFactory: () => (ws = new FakeWS()) });
  sock.connect();
  ws.onopen();                          // first open (normal)
  ws.onclose();                         // connection DROPPED (unintentional) → stale
  // queue a NoteOn and its NoteOff while disconnected
  sock.send({ type: "noteOn", channel: 1, note: 60, velocity: 100 });
  sock.send({ type: "noteOff", channel: 1, note: 60 });
  ws.sent = [];
  ws.onopen();                          // reconnect flush → clean slate
  const kinds = ws.sent.map((s) => JSON.parse(s).type);
  assert.deepEqual(kinds, ["panic"]);   // no replayed NoteOn; just an All-Notes-Off
  if (sock._timer) { clearTimeout(sock._timer); sock._timer = null; }
});

test("ws: intentional close does not schedule a reconnect (#42)", () => {
  let count = 0;
  const sock = new MidiSocket("ws://x", { socketFactory: () => { count++; return new FakeWS(); } });
  sock.connect();               // count = 1
  sock.close();                 // intentional
  if (sock.ws.onclose) sock.ws.onclose();
  assert.equal(sock._timer, null);   // no pending reconnect timer
  assert.equal(count, 1);            // no extra socket created
});

test("wizard: buildConfigPatch carries every choice (#39)", () => {
  const w = new Wizard({ name: "Alto", channel: 3, noteMin: 50, noteMax: 80, motionType: "stepper" });
  w.set({ wiring: { stepPin: 32, dirPin: 33, enablePin: 25, endstopPin: 34 }, travelMm: 120, airPreset: 2, midiSource: "din" });
  const p = w.buildConfigPatch();
  assert.equal(p.name, "Alto");
  assert.equal(p.channel, 3);
  assert.equal(p.noteMin, 50);
  assert.equal(p.motion.type, 1);            // stepper
  assert.equal(p.motion.stepper.stepPin, 32);
  assert.equal(p.motion.travelMm, 120);
  assert.equal(p.airPreset, 2);
  assert.equal(p.midiSource, "din");
});

test("wizard: air step accepts preset 0 (falsy-index bug #14.2)", () => {
  const w = new Wizard({ name: "Alto", channel: 3, noteMin: 50, noteMax: 80, motionType: "disabled" });
  w.set({ travelMm: 100, airPreset: 0 });
  // Drive the step pointer to the air step and confirm Next is allowed.
  while (w.step() !== "air" && w.next()) { /* advance */ }
  assert.equal(w.step(), "air");
  assert.equal(w.canNext(), true);        // preset 0 must not block
  w.set({ airPreset: null });
  assert.equal(w.canNext(), false);       // absent preset still blocks
});

// --- macros.js -------------------------------------------------------------
test("macros: stop at first failing step (#24)", async () => {
  const ran = [];
  const res = await runMacro([
    { name: "a", run: async () => ran.push("a") },
    { name: "b", run: async () => { ran.push("b"); throw new Error("boom"); } },
    { name: "c", run: async () => ran.push("c") },
  ]);
  assert.equal(res.ok, false);
  assert.equal(res.completed, 1);
  assert.deepEqual(ran, ["a", "b"]);   // "c" never ran
});

// --- dom.js ----------------------------------------------------------------
function fakeDoc() {
  return {
    createElement(tag) {
      return {
        tag, children: [], attrs: {}, listeners: {}, dataset: {}, _text: "",
        className: "", set textContent(v) { this._text = v; }, get textContent() { return this._text; },
        setAttribute(k, v) { this.attrs[k] = v; },
        addEventListener(e, f) { this.listeners[e] = f; },
        appendChild(c) { this.children.push(c); return c; },
        removeChild(c) { this.children = this.children.filter((x) => x !== c); },
        get firstChild() { return this.children[0]; },
      };
    },
    createTextNode(t) { return { text: t }; },
  };
}
test("dom: h() uses textContent, never innerHTML (#25)", () => {
  const doc = fakeDoc();
  const el = h("div", { text: '<img src=x onerror=alert(1)>', class: "n" },
    [h("span", { text: "child" }, [], doc)], doc);
  assert.equal(el._text, "<img src=x onerror=alert(1)>");   // stored as text, not HTML
  assert.equal(el.className, "n");
  assert.equal("innerHTML" in el, false);
});
test("dom: diffKeys computes add/remove/keep (#26)", () => {
  const d = diffKeys(["a", "b", "c"], ["b", "c", "d"]);
  assert.deepEqual(d.add, ["d"]);
  assert.deepEqual(d.remove, ["a"]);
  assert.deepEqual(d.keep, ["b", "c"]);
});
test("dom: patchText only writes on change", () => {
  const node = { textContent: "1" };
  assert.equal(patchText(node, "1"), false);
  assert.equal(patchText(node, "2"), true);
  assert.equal(node.textContent, "2");
});

// --- config.js -------------------------------------------------------------
test("config: hardware change → restart, dynamic change → not", () => {
  const base = { device: { board: 0 }, instrumentCount: 1, instruments: [
    { motion: { type: 1, stepper: { stepPin: 32, dirPin: 33, enablePin: 25, endstopMin: { pin: 34 } },
                servoA: {}, servoB: {} },
      air: { source: { type: 0 }, gate: { type: 1, pin: 27 }, flow: {}, sensor: {} } }] };
  const dyn = JSON.parse(JSON.stringify(base)); dyn.instruments[0].motion.travelMm = 120;
  assert.equal(configNeedsRestart(base, dyn), false);
  const hw = JSON.parse(JSON.stringify(base)); hw.instruments[0].motion.stepper.stepPin = 14;
  assert.equal(configNeedsRestart(base, hw), true);
});

test("config: network / MIDI-transport / angle changes need restart (#4)", () => {
  const base = { device: { board: 0 }, network: { requireAuth: true, apEnabled: true }, midi: { ble: false },
    instrumentCount: 1, instruments: [{ motion: {}, air: { source: {}, gate: {}, flow: {}, angle: { enabled: false }, sensor: {} } }] };
  const net = JSON.parse(JSON.stringify(base)); net.network.requireAuth = false;
  assert.equal(configNeedsRestart(base, net), true);
  const ble = JSON.parse(JSON.stringify(base)); ble.midi.ble = true;
  assert.equal(configNeedsRestart(base, ble), true);
  const ang = JSON.parse(JSON.stringify(base)); ang.instruments[0].air.angle.enabled = true;
  assert.equal(configNeedsRestart(base, ang), true);
  const flowt = JSON.parse(JSON.stringify(base)); flowt.instruments[0].air.flow.type = 2;
  assert.equal(configNeedsRestart(base, flowt), true);
  // a pure tuning change stays dynamic
  const tune = JSON.parse(JSON.stringify(base)); tune.midi.transpose = 3;
  assert.equal(configNeedsRestart(base, tune), false);
});
test("config: unsaved tracker", () => {
  const t = new UnsavedTracker({ a: 1 });
  assert.equal(t.isDirty({ a: 1 }), false);
  assert.equal(t.isDirty({ a: 2 }), true);
  t.markSaved({ a: 2 });
  assert.equal(t.isDirty({ a: 2 }), false);
});

// --- wizard.js -------------------------------------------------------------
test("wizard: cannot advance past an incomplete step", () => {
  const w = new Wizard();
  w.set({ name: "", channel: 1, noteMin: 48, noteMax: 84 });
  assert.equal(w.canNext(), false);      // missing name
  assert.equal(w.next(), false);
  w.set({ name: "Sopranino" });
  assert.equal(w.next(), true);
  assert.equal(w.step(), "motion");
});
test("wizard: wiring rejects duplicate GPIO", () => {
  const w = new Wizard({ name: "F", motionType: "stepper" });
  w.index = 2;                            // wiring step
  w.set({ wiring: { stepPin: 32, dirPin: 32, enablePin: 25 } });
  assert.equal(w.canNext(), false);       // dup pin 32
  w.set({ wiring: { stepPin: 32, dirPin: 33, enablePin: 25 } });
  assert.equal(w.canNext(), true);
});
test("wizard: motion enum mapping", () => {
  assert.equal(new Wizard({ motionType: "dual" }).motionEnum(), 3);
  assert.equal(new Wizard({ motionType: "disabled" }).motionEnum(), 0);
});

test("config: deepMerge preserves sibling nested fields (#14.4)", () => {
  const base = { motion: { type: 0, travelMm: 120, stepper: { stepPin: 1, dirPin: 2, microsteps: 16 } } };
  const patch = { motion: { type: 1, stepper: { stepPin: 9 } } };
  const out = deepMerge(base, patch);
  assert.equal(out.motion.type, 1);              // overridden
  assert.equal(out.motion.travelMm, 120);        // preserved
  assert.equal(out.motion.stepper.stepPin, 9);   // overridden
  assert.equal(out.motion.stepper.dirPin, 2);    // preserved (shallow merge would drop it)
  assert.equal(out.motion.stepper.microsteps, 16);
});

test("config: midiFlagsFor maps the wizard source (#14.3)", () => {
  assert.deepEqual(midiFlagsFor("din"), { din: true });
  assert.deepEqual(midiFlagsFor("serial"), { din: true });
  assert.deepEqual(midiFlagsFor("ble"), { ble: true });
  assert.deepEqual(midiFlagsFor("webKeyboard"), { webKeyboard: true });
  assert.deepEqual(midiFlagsFor("nope"), {});
});

test("config: applyWizardPatch carries motion deep-merge + midiSource (#14.3/#14.4)", () => {
  const config = {
    instrumentCount: 1,
    midi: { din: false, ble: false },
    instruments: [{ motion: { travelMm: 100, stepper: { stepPin: 1, dirPin: 2, microsteps: 32 } }, air: { source: { type: 3 } } }],
  };
  const patch = {
    name: "Alto", channel: 4, noteMin: 50, noteMax: 80,
    motion: { type: 1, stepper: { stepPin: 21 } },
    airPreset: 0, midiSource: "ble",
  };
  const out = applyWizardPatch(config, patch);
  assert.equal(out.instruments[0].name, "Alto");
  assert.equal(out.instruments[0].enabled, true);
  assert.equal(out.instruments[0].channel, 4);       // firmware schema key (#5 §17)
  assert.equal(out.instruments[0].midiChannel, undefined);
  assert.equal(out.instruments[0].motion.type, 1);
  assert.equal(out.instruments[0].motion.travelMm, 100);            // preset field kept
  assert.equal(out.instruments[0].motion.stepper.stepPin, 21);      // wizard override
  assert.equal(out.instruments[0].motion.stepper.dirPin, 2);        // preset field kept
  assert.equal(out.instruments[0].motion.stepper.microsteps, 32);   // preset field kept
  assert.equal(out.instruments[0].air.source.type, 3);              // untouched
  assert.equal(out.midi.ble, true);                                 // MIDI source applied
});

test("config: applyWizardPatch makes the MIDI source exclusive (#6 UI)", () => {
  // Start with several transports already on: choosing one source in the wizard
  // must turn the others OFF, not just OR-in the new one.
  const config = {
    instrumentCount: 1,
    midi: { din: true, ble: false, rtp: true, usb: true, webKeyboard: true },
    instruments: [{ motion: {}, air: {} }],
  };
  const out = applyWizardPatch(config, { name: "X", channel: 1, midiSource: "ble" });
  assert.equal(out.midi.ble, true);          // chosen source on
  assert.equal(out.midi.din, false);         // others cleared
  assert.equal(out.midi.rtp, false);
  assert.equal(out.midi.usb, false);
  assert.equal(out.midi.webKeyboard, false);

  // An empty/omitted source leaves the transport flags untouched.
  const keep = applyWizardPatch(config, { name: "X", channel: 1, midiSource: "" });
  assert.equal(keep.midi.din, true);
  assert.equal(keep.midi.rtp, true);
});
