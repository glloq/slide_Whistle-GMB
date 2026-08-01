// Standalone native test of the REAL EspMotionSink backend (compiled with a
// minimal Arduino stub, -DARDUINO), not a fake IMotionSink. Review #8 §1: the
// backend must reset its executed-step counter on syncPositionMm() so the first
// post-homing move does not drive a phantom correction back to the pre-home
// count. This binary is separate from the main suite because EspSinks.h only
// exists under ARDUINO.
#define ARDUINO 300
#include <Arduino.h>
#include <cstdio>
#include "../../esp32/esp32_slide_whistle/core/platform/EspSinks.h"

using namespace swc;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    SlideMotionConfig mc;
    mc.type = SlideDriveType::StepDir;
    mc.stepper.stepPin = 12; mc.stepper.dirPin = 13; mc.stepper.enablePin = -1;
    mc.stepper.stepsPerMm = 80.0f; mc.stepper.invertDir = false;
    g_stepPin = mc.stepper.stepPin;

    EspMotionSink sink; sink.begin(mc);

    // Simulate a homing seek that moved the slide to +5 mm: curSteps_ climbs to
    // 5*80 = 400 across several bounded writeStepperMm() calls.
    for (int i = 0; i < 100; ++i) sink.writeStepperMm(5.0f);
    g_stepPulses = 0;
    // A repeat of the SAME commanded position now emits no steps (delta 0) —
    // confirms curSteps_ tracked the 5 mm.
    sink.writeStepperMm(5.0f);
    CHECK(g_stepPulses == 0);

    // Homing contact: the actuator redefines the reference to 0 mm and calls
    // syncPositionMm(0). The real backend must zero curSteps_.
    sink.syncPositionMm(0.0f);
    g_stepPulses = 0;
    // The next commanded move to 0 mm must therefore produce ZERO steps — no
    // phantom 400-step correction back to the old count.
    for (int i = 0; i < 100; ++i) sink.writeStepperMm(0.0f);
    CHECK(g_stepPulses == 0);

    // And a real move away from the new zero still steps normally: 2 mm = 160
    // steps, emitted over bounded calls.
    g_stepPulses = 0;
    for (int i = 0; i < 100; ++i) sink.writeStepperMm(2.0f);
    CHECK(g_stepPulses == 160);

    // Homing toward the max end defines the reference at travelMm — the counter
    // must jump to travelMm*stepsPerMm so a later move inward doesn't unwind it.
    sink.syncPositionMm(100.0f);
    g_stepPulses = 0;
    sink.writeStepperMm(100.0f);
    CHECK(g_stepPulses == 0);

    if (failures == 0) std::printf("==== EspMotionSink backend: all checks passed ====\n");
    else               std::printf("==== EspMotionSink backend: %d FAILURES ====\n", failures);
    return failures ? 1 : 0;
}
