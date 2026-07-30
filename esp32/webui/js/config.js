/*
 * config.js — client mirror of the firmware's dynamic-vs-restart rule so the UI
 * can warn about restart_required before saving (Section 10/13), and a small
 * unsaved-changes tracker.
 *
 * The firmware remains the source of truth: its POST /api/v1/config response
 * carries the authoritative restart_required. This mirror is only for instant
 * client feedback.
 */

// Fields whose change needs a hardware restart.
const HW_PATHS = [
  "device.board",
  "motion.type",
  "motion.stepper.stepPin", "motion.stepper.dirPin", "motion.stepper.enablePin",
  "motion.stepper.endstopMin.pin",
  "motion.servoA.pin", "motion.servoA.backend", "motion.servoA.pcaChannel",
  "motion.servoB.pin", "motion.servoB.backend", "motion.servoB.pcaChannel",
  "air.source.type", "air.gate.type", "air.gate.pin", "air.gate.backend",
  "air.flow.pin", "air.flow.backend", "air.sensor.type", "air.sensor.pin",
];

function get(obj, path) {
  return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

export function instrumentNeedsRestart(oldInst, newInst) {
  if (!oldInst || !newInst) return true;
  for (const p of HW_PATHS) {
    const a = p.startsWith("device.") ? undefined : get(oldInst, p);
    const b = p.startsWith("device.") ? undefined : get(newInst, p);
    if (JSON.stringify(a) !== JSON.stringify(b)) return true;
  }
  return false;
}

export function configNeedsRestart(oldCfg, newCfg) {
  if (!oldCfg || !newCfg) return true;
  if (get(oldCfg, "device.board") !== get(newCfg, "device.board")) return true;
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
  inst.midiChannel = patch.channel;
  inst.noteMin = patch.noteMin;
  inst.noteMax = patch.noteMax;
  inst.motion = deepMerge(prev.motion || {}, patch.motion || {});
  insts[0] = inst;
  cfg.instruments = insts;
  if (!cfg.instrumentCount || cfg.instrumentCount < 1) cfg.instrumentCount = insts.length;
  if (patch.midiSource != null && patch.midiSource !== "")
    cfg.midi = { ...(cfg.midi || {}), ...midiFlagsFor(patch.midiSource) };
  return cfg;
}

// Tracks whether the working config differs from the last-saved one.
export class UnsavedTracker {
  constructor(saved) { this.saved = JSON.stringify(saved ?? null); }
  markSaved(cfg) { this.saved = JSON.stringify(cfg); }
  isDirty(cfg) { return JSON.stringify(cfg) !== this.saved; }
}
