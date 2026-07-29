/*
 * macros.js — run a sequence of async steps, stopping at the first failure.
 *
 * A macro (e.g. the wizard's "wire → test direction → home → calibrate") must
 * halt the moment a step throws — it must never barrel on after an error
 * (correction #24).
 */

export async function runMacro(steps, { onStep = () => {} } = {}) {
  const results = [];
  for (let i = 0; i < steps.length; i++) {
    const step = steps[i];
    onStep({ index: i, name: step.name, phase: "start" });
    try {
      const value = await step.run();
      results.push({ name: step.name, ok: true, value });
      onStep({ index: i, name: step.name, phase: "ok", value });
    } catch (err) {
      results.push({ name: step.name, ok: false, error: err });
      onStep({ index: i, name: step.name, phase: "error", error: err });
      return { ok: false, completed: i, total: steps.length, results, error: err };
    }
  }
  return { ok: true, completed: steps.length, total: steps.length, results };
}
