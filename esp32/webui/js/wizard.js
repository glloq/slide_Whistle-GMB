/*
 * wizard.js — first-boot / simple-mode setup state machine (Section 13).
 *
 * Pure logic (no DOM) so the step flow and gating are unit-tested: you cannot
 * advance past a step whose required fields are missing, and the accumulated
 * data builds an instrument config for POST /api/v1/config.
 */

export const WIZARD_STEPS = [
  "instrument", // name, channel, note range, count
  "motion",     // stepper / single servo / dual servo / disabled
  "wiring",     // GPIO / PCA9685 backend
  "mechCal",    // limits, travel, homing
  "air",        // air mounting card
  "musicCal",   // note table
  "validate",   // MIDI source + review + save
];

const VALIDATORS = {
  instrument: (d) => !!d.name && d.channel >= 0 && d.channel <= 16 && d.noteMin < d.noteMax,
  motion: (d) => ["stepper", "single", "dual", "disabled"].includes(d.motionType),
  wiring: (d) => wiringComplete(d),
  mechCal: (d) => d.travelMm > 0,
  air: (d) => !!d.airPreset,
  musicCal: () => true,   // may be skipped / auto-generated
  validate: (d) => d.midiSource != null,
};

function wiringComplete(d) {
  if (d.motionType === "disabled") return true;
  const pins = collectPins(d);
  if (pins.some((p) => p == null || p < 0)) return false;
  return new Set(pins).size === pins.length;   // no duplicate GPIO
}

function collectPins(d) {
  const w = d.wiring || {};
  switch (d.motionType) {
    case "stepper": return [w.stepPin, w.dirPin, w.enablePin].filter((p) => p !== undefined);
    case "single":  return [w.servoAPin];
    case "dual":    return [w.servoAPin, w.servoBPin];
    default:        return [];
  }
}

export class Wizard {
  constructor(data = {}) {
    this.index = 0;
    this.data = { channel: 1, noteMin: 48, noteMax: 84, count: 1, ...data };
  }
  step() { return WIZARD_STEPS[this.index]; }
  isFirst() { return this.index === 0; }
  isLast() { return this.index === WIZARD_STEPS.length - 1; }
  canNext() { return (VALIDATORS[this.step()] || (() => true))(this.data); }
  set(patch) { Object.assign(this.data, patch); return this; }

  next() {
    if (!this.canNext() || this.isLast()) return false;
    this.index++; return true;
  }
  prev() {
    if (this.isFirst()) return false;
    this.index--; return true;
  }

  // Map wizard answers → the motion.type the firmware expects.
  motionEnum() {
    return { disabled: 0, stepper: 1, single: 2, dual: 3 }[this.data.motionType] ?? 0;
  }
}
