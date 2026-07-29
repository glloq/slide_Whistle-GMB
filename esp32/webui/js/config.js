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

// Tracks whether the working config differs from the last-saved one.
export class UnsavedTracker {
  constructor(saved) { this.saved = JSON.stringify(saved ?? null); }
  markSaved(cfg) { this.saved = JSON.stringify(cfg); }
  isDirty(cfg) { return JSON.stringify(cfg) !== this.saved; }
}
