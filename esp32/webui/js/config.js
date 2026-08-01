/*
 * config.js — client mirror of the firmware's dynamic-vs-restart rule so the UI
 * can warn about restart_required before saving (Section 10/13), and a small
 * unsaved-changes tracker.
 *
 * The firmware remains the source of truth: its POST /api/v1/config response
 * carries the authoritative restart_required. This mirror is only for instant
 * client feedback.
 */

// Per-instrument fields whose change needs a hardware restart (mirror of the
// firmware's configNeedsRestart — keep the two in sync, review #4 §P1).
const HW_PATHS = [
  "enabled",
  "motion.type",
  "motion.stepper.stepPin", "motion.stepper.dirPin", "motion.stepper.enablePin",
  "motion.stepper.endstopMin.pin", "motion.stepper.endstopMax.pin",
  "motion.servoA.pin", "motion.servoA.backend", "motion.servoA.pca",
  "motion.servoB.pin", "motion.servoB.backend", "motion.servoB.pca",
  "air.source.type", "air.source.pumpCount",
  "air.gate.type", "air.gate.pin", "air.gate.backend", "air.gate.pca",
  "air.flow.type", "air.flow.pin", "air.flow.backend", "air.flow.pca",
  "air.angle.enabled", "air.angle.pin", "air.angle.backend", "air.angle.pca",
  "air.sensor.type", "air.sensor.pin",
  // Parameters applyDynamic() cannot push to the built objects, so a change
  // needs a reboot too (mirror of the firmware's unappliedParamsDiffer, review
  // #7 §12). The firmware response stays authoritative; this only sharpens the
  // instant client-side hint. Kept to the fields the UI commonly edits.
  "motion.travelMm",
  "motion.stepper.stepsPerMm", "motion.stepper.invertDir", "motion.stepper.homeTowardZero",
  "motion.stepper.idleDisableMs", "motion.stepper.alwaysHold",
  "air.source.spinUpMs", "air.source.pidKp", "air.source.requireSensor",
  "air.gate.servoMinUs", "air.gate.servoMaxUs", "air.gate.activeHigh",
  "air.flow.servoMinUs", "air.flow.servoMaxUs",
  "air.sensor.staleTimeoutMs", "watchdogMs",
];

// Device/config-level fields (outside the per-instrument array) needing restart:
// network + auth re-init and MIDI transport bring-up (transpose stays dynamic).
const CONFIG_HW_PATHS = [
  "device.board",
  "network.apEnabled", "network.apSsid", "network.requireAuth",
  "network.disableApWhenConnected", "network.allowedOrigin",
  "midi.din", "midi.ble", "midi.rtp", "midi.usb", "midi.webKeyboard",
];

function get(obj, path) {
  return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

export function instrumentNeedsRestart(oldInst, newInst) {
  if (!oldInst || !newInst) return true;
  for (const p of HW_PATHS) {
    if (JSON.stringify(get(oldInst, p)) !== JSON.stringify(get(newInst, p))) return true;
  }
  return false;
}

export function configNeedsRestart(oldCfg, newCfg) {
  if (!oldCfg || !newCfg) return true;
  for (const p of CONFIG_HW_PATHS) {
    if (JSON.stringify(get(oldCfg, p)) !== JSON.stringify(get(newCfg, p))) return true;
  }
  if (oldCfg.instrumentCount !== newCfg.instrumentCount) return true;
  const n = newCfg.instrumentCount || (newCfg.instruments || []).length;
  for (let i = 0; i < n; i++) {
    if (instrumentNeedsRestart(oldCfg.instruments?.[i], newCfg.instruments?.[i])) return true;
  }
  return false;
}

// Recursive merge of plain objects: nested objects are merged key-by-key rather
// than replaced wholesale, so applying a wizard patch that only sets
// motion.stepper.stepPin keeps the preset's other stepper fields (#3 §14.4).
export function deepMerge(base, patch) {
  const isObj = (v) => v && typeof v === "object" && !Array.isArray(v);
  if (!isObj(base) || !isObj(patch)) return patch === undefined ? base : patch;
  const out = { ...base };
  for (const k of Object.keys(patch)) {
    out[k] = isObj(out[k]) && isObj(patch[k]) ? deepMerge(out[k], patch[k]) : patch[k];
  }
  return out;
}

// Map the wizard's single midiSource choice onto the config.midi transport
// flags (#3 §14.3). Unknown/empty leaves the flags untouched.
export function midiFlagsFor(source) {
  switch (source) {
    case "serial":
    case "din":         return { din: true };
    case "ble":         return { ble: true };
    case "rtp":         return { rtp: true };
    case "usb":         return { usb: true };
    case "web":
    case "webKeyboard": return { webKeyboard: true };
    default:            return {};
  }
}

// Apply a wizard buildConfigPatch() onto a full config, in place-safe fashion
// (returns a new config object). Deep-merges motion and applies the MIDI source
// so NO wizard choice is silently dropped (#3 §14.3/§14.4). Pure + unit-tested.
export function applyWizardPatch(config, patch) {
  const cfg = { ...(config || {}) };
  const insts = Array.isArray(cfg.instruments) ? cfg.instruments.slice() : [];
  const prev = insts[0] || {};
  const inst = { ...prev };
  inst.enabled = true;
  inst.name = patch.name;
  // The firmware config schema keys the MIDI channel as "channel" (decoded into
  // midiChannel); writing "midiChannel" here would be silently dropped on decode
  // and the old channel would stay (review #5 §17).
  inst.channel = patch.channel;
  inst.noteMin = patch.noteMin;
  inst.noteMax = patch.noteMax;
  inst.motion = deepMerge(prev.motion || {}, patch.motion || {});
  insts[0] = inst;
  cfg.instruments = insts;
  if (!cfg.instrumentCount || cfg.instrumentCount < 1) cfg.instrumentCount = insts.length;
  if (patch.midiSource != null && patch.midiSource !== "")
    // The wizard picks ONE MIDI source: clear the other transports first so
    // choosing e.g. BLE doesn't leave DIN/RTP/WebKeyboard on (review #6 UI).
    cfg.midi = { ...(cfg.midi || {}),
                 din: false, ble: false, rtp: false, usb: false, webKeyboard: false,
                 ...midiFlagsFor(patch.midiSource) };
  return cfg;
}

// Tracks whether the working config differs from the last-saved one.
export class UnsavedTracker {
  constructor(saved) { this.saved = JSON.stringify(saved ?? null); }
  markSaved(cfg) { this.saved = JSON.stringify(cfg); }
  isDirty(cfg) { return JSON.stringify(cfg) !== this.saved; }
}
