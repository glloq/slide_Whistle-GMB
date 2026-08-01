// Standalone native test of the REAL EspMotionSink backend (compiled with a
// minimal Arduino stub, -DARDUINO), not a fake IMotionSink. It exercises the
// hardware-timer step generator's pulse-EMISSION LOGIC by driving serviceTick()
// directly (the actual timer ISR that calls it on hardware is not modelled —
// that part is bench-verified). Review #7/#8 §1/§6: the executed-step counter is
// realigned at a homing sync, and executedPositionMm() trails then converges as
// pulses are emitted. This binary is separate from the main suite because
// EspSinks.h only exists under ARDUINO.
#define ARDUINO 300
#include <Arduino.h>
#include <cstdio>
#include "../../esp32/esp32_slide_whistle/core/platform/EspSinks.h"

using namespace swc;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Emulate the timer ISR: run the step generator for n ticks (2 ticks per pulse).
static void driveTicks(EspMotionSink& s, int n) { for (int i = 0; i < n; ++i) s.serviceStepGenTick(); }

int main() {
    SlideMotionConfig mc;
    mc.type = SlideDriveType::StepDir;
    mc.stepper.stepPin = 12; mc.stepper.dirPin = 13; mc.stepper.enablePin = -1;
    mc.stepper.stepsPerMm = 80.0f; mc.stepper.invertDir = false;
    g_stepPin = mc.stepper.stepPin;

    EspMotionSink sink; sink.begin(mc);

    // writeStepperMm only PUBLISHES a target — non-blocking, no pulses yet.
    g_stepPulses = 0;
    sink.writeStepperMm(5.0f);                 // target = 5 mm = 400 steps
    CHECK(g_stepPulses == 0);                  // nothing emitted until the ISR runs
    float exec = -1.0f;
    CHECK(sink.executedPositionMm(exec));
    CHECK(exec == 0.0f);                        // executed still at 0

    // Drive the generator: 2 ticks per step, so 800 ticks emits 400 steps.
    driveTicks(sink, 800);
    CHECK(g_stepPulses == 400);                // exactly 400 pulses on the step pin
    CHECK(sink.executedPositionMm(exec));
    CHECK(exec > 4.99f && exec < 5.01f);       // executed converged to 5 mm

    // Homing sync redefines the reference to 0 with NO motion; a subsequent
    // command to 0 emits nothing (no phantom correction back to the old count).
    sink.syncPositionMm(0.0f);
    CHECK(sink.executedPositionMm(exec)); CHECK(exec == 0.0f);
    g_stepPulses = 0;
    sink.writeStepperMm(0.0f);
    driveTicks(sink, 400);
    CHECK(g_stepPulses == 0);

    // Executed position TRAILS during a move (review #7/#8 §6): after only a few
    // ISR ticks the executed counter is far short of the 50 mm target.
    sink.writeStepperMm(50.0f);                // target = 4000 steps
    driveTicks(sink, 20);                      // ~10 steps emitted
    CHECK(sink.executedPositionMm(exec));
    CHECK(exec > 0.0f && exec < 1.0f);         // trailing, nowhere near 50 mm
    driveTicks(sink, 8000);                    // let it finish
    CHECK(sink.executedPositionMm(exec));
    CHECK(exec > 49.99f && exec < 50.01f);     // converged

    // Homing toward the max end defines the reference at travelMm; commanding it
    // again moves nothing.
    sink.syncPositionMm(100.0f);
    g_stepPulses = 0;
    sink.writeStepperMm(100.0f);
    driveTicks(sink, 400);
    CHECK(g_stepPulses == 0);

    if (failures == 0) std::printf("==== EspMotionSink backend: all checks passed ====\n");
    else               std::printf("==== EspMotionSink backend: %d FAILURES ====\n", failures);
    return failures ? 1 : 0;
}
