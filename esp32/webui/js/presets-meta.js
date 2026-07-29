/*
 * presets-meta.js — UI metadata for the 11 mounting presets. The `index`
 * values match PresetId in the firmware (core/Presets.h). Descriptive text
 * only — the firmware fills the actual parameters.
 */
export const presetCatalog = [
  { index: 0, name: "Stepper + fan + diverter", hint: "Historic Fan-Servo mount" },
  { index: 1, name: "Stepper + PWM solenoid + flow", hint: "Historic Solenoid-Servo mount" },
  { index: 2, name: "Stepper + solenoid only", hint: "External air, fixed flow" },
  { index: 3, name: "Stepper + servo valve + flow", hint: "Servo valve gate" },
  { index: 4, name: "Stepper + flow servo as valve", hint: "One servo does both" },
  { index: 5, name: "Stepper + PWM fan + flow", hint: "Variable fan source" },
  { index: 6, name: "Stepper + pumps + valve", hint: "1–3 direct pumps" },
  { index: 7, name: "Stepper + pumps + tank + sensor", hint: "Regulated reservoir" },
  { index: 8, name: "Single slide servo + minimal air", hint: "Servo-driven slide" },
  { index: 9, name: "Dual slide servos + minimal air", hint: "Two synchronised servos" },
  { index: 10, name: "Fully custom", hint: "Start from a blank, safe config" },
];
