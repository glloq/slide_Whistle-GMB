/*
 * app.js — universal controller UI entry point.
 *
 * Wires the tested logic modules (api / notes / ws / dom / config) into a
 * responsive, XSS-safe interface. All hardware actions go through /api/v1 or
 * the WebSocket — the page never assumes success and always shows firmware
 * errors. The PANIC button is always visible; browser note events are flushed
 * on blur / disconnect.
 */
import { makeApi, ApiError } from "./js/api.js";
import { NoteRegistry, bindLifecycleFlush } from "./js/notes.js";
import { MidiSocket } from "./js/ws.js";
import { h, clear, patchText } from "./js/dom.js";
import { presetCatalog } from "./js/presets-meta.js";
import { Wizard, WIZARD_STEPS } from "./js/wizard.js";
import { applyWizardPatch } from "./js/config.js";

const state = {
  // Only the SESSION token is kept in the browser — never the admin secret (#5).
  session: sessionStorage.getItem("session") || "",
  config: null,
  tab: "play",
  wizard: new Wizard(),
};
const api = makeApi({ getToken: () => state.session });
const socket = new MidiSocket(wsUrl(), { token: state.session }).connect();

async function doLogin(adminToken) {
  const r = await guard(() => api.login(adminToken), "Logged in");
  if (r && r.session) {
    state.session = r.session;
    sessionStorage.setItem("session", r.session);   // session only, not the admin token
    socket.token = r.session;
    socket.close(); socket.connect();                // re-auth the WebSocket
    await refreshConfig().catch(() => {});           // reload config now we're authed (#41)
    render();
  }
}
function loginCard() {
  const input = h("input", { type: "password", placeholder: "admin token (shown on Serial at boot)" });
  return h("div", { class: "card" }, [
    h("h2", { text: "Administrator login" }),
    h("p", { class: "muted", text: "Enter the admin token printed on the device's serial console to unlock configuration." }),
    input,
    h("div", { class: "row" }, [
      h("button", { class: "btn btn-primary", text: "Log in", onclick: () => doLogin(input.value.trim()) }),
    ]),
  ]);
}
const notes = new NoteRegistry((kind, m) =>
  socket.send({ type: kind, channel: m.channel, note: m.note, velocity: m.velocity ?? 100 }));
bindLifecycleFlush(notes);
socket.onStatus = (s) => { setConn(s === "open"); if (s !== "open") notes.allOff(); };
// Surface server refusals sent over the WebSocket (e.g. 401 on a keyboard note)
// instead of silently swallowing them (review item #40).
socket.onMessage = (msg) => {
  if (msg && msg.ok === false && msg.error)
    toast(`${msg.error.code || "error"}: ${msg.error.message || "refused"}`, "err");
};

const $ = (id) => document.getElementById(id);

// ---- error / toast helpers ------------------------------------------------
function toast(message, kind = "ok") {
  const box = $("toasts");
  const t = h("div", { class: `toast ${kind}`, text: message });
  box.appendChild(t);
  setTimeout(() => t.remove(), 4200);
}
async function guard(fn, okMsg) {
  try { const r = await fn(); if (okMsg) toast(okMsg, "ok"); return r; }
  catch (e) {
    if (e instanceof ApiError) toast(`${e.code || e.status}: ${e.message}${e.field ? " (" + e.field + ")" : ""}`, "err");
    else toast(String(e.message || e), "err");
    throw e;   // stop any macro/caller (#24)
  }
}

// ---- top bar --------------------------------------------------------------
function setConn(on) {
  const el = $("conn");
  el.dataset.state = on ? "on" : "off";
  el.className = "pill " + (on ? "pill-on" : "pill-off");
  el.textContent = on ? "online" : "offline";
}
$("panic").addEventListener("click", () => {
  notes.allOff();
  socket.send({ type: "panic" });
  guard(() => api.command({ type: "panic" }), "Panic sent").catch(() => {});
});

// ---- tabs -----------------------------------------------------------------
const TABS = [
  ["play", "Play"], ["setup", "Setup"], ["expert", "Expert"], ["diag", "Diagnostics"],
];
function renderTabs() {
  const nav = $("tabs"); clear(nav);
  for (const [id, label] of TABS) {
    nav.appendChild(h("button", {
      class: "btn", text: label, "aria-selected": String(state.tab === id),
      onclick: () => { state.tab = id; render(); },
    }));
  }
}

// ---- views ----------------------------------------------------------------
function render() {
  renderTabs();
  const view = $("view"); clear(view);
  ({ play: viewPlay, setup: viewSetup, expert: viewExpert, diag: viewDiag }[state.tab] || viewPlay)(view);
}

function viewPlay(root) {
  root.appendChild(h("div", { class: "card" }, [
    h("h2", { text: "Web keyboard" }),
    h("p", { class: "muted", text: "Notes are sent over WebSocket and released automatically if the tab loses focus." }),
    buildKeyboard(60, 17),
  ]));
}

function buildKeyboard(start, count) {
  const kbd = h("div", { class: "kbd" });
  const blackSet = new Set([1, 3, 6, 8, 10]);
  for (let i = 0; i < count; i++) {
    const note = start + i;
    const isBlack = blackSet.has(note % 12);
    const key = h("button", { class: "key" + (isBlack ? " black" : ""), "data-note": note, "aria-label": "note " + note });
    const on = (ev) => { ev.preventDefault(); key.classList.add("down"); notes.noteOn(1, note, 100); };
    const off = () => { key.classList.remove("down"); notes.noteOff(1, note); };
    key.addEventListener("pointerdown", on);
    key.addEventListener("pointerup", off);
    key.addEventListener("pointerleave", off);
    key.addEventListener("pointercancel", off);
    kbd.appendChild(key);
  }
  return kbd;
}

function viewSetup(root) {
  if (!state.session) { root.appendChild(loginCard()); return; }   // must log in first
  root.appendChild(wizardCard());
  root.appendChild(h("div", { class: "card" }, [
    h("h2", { text: "Air mounting presets" }),
    h("p", { class: "muted", text: "Pick a mounting; it fills sane defaults you can still edit in Expert mode." }),
    (() => {
      const grid = h("div", { class: "grid" });
      presetCatalog.forEach((p) => grid.appendChild(h("button", { class: "btn preset", onclick: () => applyPreset(p.index) }, [
        h("strong", { text: p.name }),
        h("small", { text: p.hint }),
      ])));
      return grid;
    })(),
  ]));
}

// First-boot / simple-mode wizard driven by wizard.js (gating enforced there).
function wizardCard() {
  const w = state.wizard;
  const card = h("div", { class: "card" });
  card.appendChild(h("h2", { text: `Setup wizard — step ${w.index + 1}/${WIZARD_STEPS.length}: ${w.step()}` }));
  const body = h("div");
  const step = w.step();
  if (step === "instrument") {
    body.appendChild(field("Name", h("input", { value: w.data.name || "", oninput: (e) => w.set({ name: e.target.value }) })));
    body.appendChild(field("MIDI channel", numInput(w.data.channel, 0, 16, (v) => w.set({ channel: v }))));
  } else if (step === "motion") {
    for (const [id, label] of [["stepper","Stepper"],["single","One servo"],["dual","Two servos"],["disabled","No movement"]])
      body.appendChild(h("button", { class: "btn", "aria-selected": String(w.data.motionType === id),
        text: label, onclick: () => { w.set({ motionType: id }); render(); } }));
  } else if (step === "air") {
    presetCatalog.forEach((p) => body.appendChild(h("button", { class: "btn",
      "aria-selected": String(w.data.airPreset === p.index),
      text: p.name, onclick: () => { w.set({ airPreset: p.index }); render(); } })));
  } else {
    body.appendChild(h("p", { class: "muted", text: "Configure this step, then continue." }));
  }
  card.appendChild(body);
  card.appendChild(h("div", { class: "row" }, [
    h("button", { class: "btn", text: "◀ Back", onclick: () => { w.prev(); render(); } }),
    h("button", { class: "btn btn-primary", text: w.isLast() ? "Finish" : "Next ▶",
      onclick: () => { if (w.isLast()) finishWizard(); else if (w.next()) render(); else toast("Complete this step first", "err"); } }),
  ]));
  return card;
}
function field(label, node) { return h("div", {}, [h("label", { text: label }), node]); }
function numInput(val, min, max, on) {
  return h("input", { type: "number", value: String(val ?? ""), min: String(min), max: String(max),
    oninput: (e) => on(parseInt(e.target.value, 10)) });
}
async function finishWizard() {
  const w = state.wizard;
  const patch = w.buildConfigPatch();            // every collected choice (#39)
  await guard(async () => {
    if (patch.airPreset != null) await api.applyPreset(patch.airPreset, 0);
    await refreshConfig();
    // Deep-merge the wizard's instrument choices onto the (preset-filled) config
    // and apply the MIDI source, so no answer is dropped (#3 §14.3/§14.4).
    state.config = applyWizardPatch(state.config, patch);
    const r = await api.putConfig(state.config);
    if (r.restart_required) showRestart(true);
    await refreshConfig();
  }, "Wizard applied");
  state.tab = "diag"; render();
}

async function applyPreset(index) {
  await guard(async () => {
    const r = await api.applyPreset(index, 0);
    if (r.restart_required) showRestart(true);
    await refreshConfig();
  }, "Preset applied");
}

function viewExpert(root) {
  if (!state.session) { root.appendChild(loginCard()); return; }   // protected
  const card = h("div", { class: "card" });
  card.appendChild(h("h2", { text: "Configuration (JSON)" }));
  const ta = h("textarea", { class: "mono", rows: "16", style: "width:100%" });
  ta.value = state.config ? JSON.stringify(state.config, null, 2) : "{}";
  card.appendChild(ta);
  card.appendChild(h("div", { class: "row" }, [
    h("button", { class: "btn btn-primary", text: "Validate & save", onclick: () => saveExpert(ta.value) }),
    h("button", { class: "btn", text: "Reload", onclick: () => refreshConfig().then(render) }),
  ]));
  root.appendChild(card);
}

async function saveExpert(text) {
  let obj;
  try { obj = JSON.parse(text); } catch (e) { toast("Invalid JSON: " + e.message, "err"); return; }
  await guard(async () => {
    const r = await api.putConfig(obj);
    if (r.restart_required) showRestart(true);
    await refreshConfig();
  }, "Configuration saved");
}

function viewDiag(root) {
  const card = h("div", { class: "card" }, [h("h2", { text: "Live status" })]);
  const list = h("div", { class: "mono", id: "statusList" });
  card.appendChild(list);
  // One Home button per CONFIGURED, enabled instrument, addressed by its stable
  // id — not a single hardcoded "instrument 0" (#3 §14.8).
  const controls = h("div", { class: "row" });
  const insts = (state.config && state.config.instruments) || [];
  let any = false;
  insts.forEach((inst, id) => {
    if (inst && inst.enabled === false) return;
    any = true;
    const label = inst && inst.name ? `Home ${inst.name}` : `Home instrument ${id}`;
    controls.appendChild(h("button", { class: "btn", text: label,
      onclick: () => guard(() => api.command({ type: "home", instrument: id }), "Homing…") }));
  });
  if (!any)
    controls.appendChild(h("button", { class: "btn", text: "Home instrument 0",
      onclick: () => guard(() => api.command({ type: "home", instrument: 0 }), "Homing…") }));
  controls.appendChild(h("button", { class: "btn", text: "Factory reset",
    onclick: () => confirm("Reset to defaults?") && guard(() => api.factoryReset(), "Reset done").then(refreshConfig) }));
  card.appendChild(controls);
  root.appendChild(card);
}

function showRestart(on) { $("restart").hidden = !on; }

// ---- status polling with differential text updates (#26) ------------------
let statusNodes = {};
async function pollStatus() {
  try {
    const s = await api.status();
    setConn(true);
    if (s.restart_required) showRestart(true);
    const list = $("statusList");
    if (list && Array.isArray(s.instruments)) {
      s.instruments.forEach((inst, i) => {
        let node = statusNodes[i];
        if (!node || !node.isConnected) {
          node = h("div"); statusNodes[i] = node; list.appendChild(node);
        }
        patchText(node, `#${i}  pos=${(inst.pos_mm ?? 0).toFixed(1)}mm  homed=${inst.homed}  note=${inst.note}`);
      });
    }
  } catch { setConn(false); }
}

async function refreshConfig() {
  const wrap = await guard(() => api.getConfig());
  state.config = wrap.config ?? wrap;
}

function wsUrl() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.host}/ws`;
}

// ---- boot -----------------------------------------------------------------
(async function boot() {
  render();
  await refreshConfig().catch(() => {});
  render();
  setInterval(pollStatus, 400);   // ~2.5 Hz general status
})();
