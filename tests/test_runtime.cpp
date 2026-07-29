/*
 * tests/test_runtime.cpp — InstrumentRuntime: build the right actuator + air
 * from config and run the full chain (config-driven, no recompile).
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/InstrumentRuntime.h"
#include "../esp32/esp32_slide_whistle/core/Presets.h"

using namespace swc;

struct MSink : IMotionSink {
    float mm = 0;
    void writeStepperMm(float v) override { mm = v; }
    void writeServoUs(uint8_t, uint16_t) override {}
    void enableDriver(bool) override {}
    bool readEndstop(bool) override { return mm <= 0.0f; }
};
struct ASink : IAirSink {
    bool gate = false; float flow = 0;
    void setSourceLevel(uint8_t, float) override {}
    void setGateOpen(bool o) override { gate = o; }
    void setGatePwm(float) override {}
    void setFlow(float v) override { flow = v; }
    void setAngle(float) override {}
    float readSensorRaw() override { return NAN; }
};

static InstrumentConfig cfgFor(PresetId p) {
    InstrumentConfig c; c.enabled = true; c.midiChannel = 1; c.noteMin = 48; c.noteMax = 84;
    applyPreset(c, p);
    for (int n = 48; n <= 84; ++n) c.map.setPoint((uint8_t)n, (n - 48) * 2.0f, 60);
    return c;
}

TEST(runtime_disabled_plays_immediately) {
    MSink m; ASink a; InstrumentRuntime rt(&m, &a);
    CHECK(rt.build(0, cfgFor(PresetId::FullyCustom)));   // Disabled motion + solenoid? FullyCustom = no gate
    // FullyCustom has gate None; use a preset with a gate for observability:
    InstrumentConfig c = cfgFor(PresetId::SingleServoMinimalAir);
    c.motion.type = SlideDriveType::Disabled;            // force disabled motion, keep solenoid gate
    CHECK(rt.build(0, c));
    uint32_t t = 0;
    rt.instrument().noteOn(60, 100, t);
    for (int k = 0; k < 3; ++k) { t++; rt.update(t, t * 1000); }
    CHECK(a.gate);   // Disabled actuator is instantly ready → air opens
}

TEST(runtime_stepper_waits_for_home) {
    MSink m; ASink a; InstrumentRuntime rt(&m, &a);
    InstrumentConfig c = cfgFor(PresetId::StepperSolenoidOnly);
    c.motion.maxSpeedMmS = 40; c.motion.accelMmS2 = 200;
    c.motion.stepper.homingFastMmS = 200; c.motion.stepper.phaseTimeoutMs = 5000;
    c.motion.stepper.homeBackoffMm = 2;
    CHECK(rt.build(0, c));
    uint32_t t = 0;
    // not homed → a note cannot open air yet
    rt.instrument().noteOn(72, 100, t);
    for (int k = 0; k < 5; ++k) { t++; rt.update(t, t * 1000); }
    CHECK(!a.gate);
    rt.instrument().allNotesOff(t);     // clear pending note (panic would e-stop)
    // home, then a note plays
    rt.actuator()->requestHoming();
    for (int k = 0; k < 3000 && !rt.actuator()->isHomed(); ++k) { t++; rt.update(t, t * 1000); }
    CHECK(rt.actuator()->isHomed());
    rt.instrument().noteOn(60, 100, t);
    for (int k = 0; k < 4000; ++k) { t++; rt.update(t, t * 1000); }
    CHECK(a.gate);                       // moved to position then opened air
    CHECK_NEAR(rt.actuator()->currentPositionMm(), 24.0f, 1.0f);   // note 60 → 24 mm
}

TEST(runtime_rebuild_switches_mechanism) {
    MSink m; ASink a; InstrumentRuntime rt(&m, &a);
    CHECK(rt.build(0, cfgFor(PresetId::StepperSolenoidOnly)));
    CHECK(rt.config().motion.type == SlideDriveType::StepDir);
    // rebuild as a single servo — no recompile, just new config
    CHECK(rt.build(0, cfgFor(PresetId::SingleServoMinimalAir)));
    CHECK(rt.config().motion.type == SlideDriveType::SingleServo);
    uint32_t t = 0;
    rt.actuator()->requestHoming();
    for (int k = 0; k < 20; ++k) { t++; rt.update(t, t * 1000); }
    CHECK(rt.actuator()->isHomed());     // servo homes instantly
    rt.instrument().noteOn(60, 100, t);
    for (int k = 0; k < 1500; ++k) { t++; rt.update(t, t * 1000); }
    CHECK(a.gate);
}

TEST(runtime_safe_state) {
    MSink m; ASink a; InstrumentRuntime rt(&m, &a);
    rt.build(0, cfgFor(PresetId::StepperSolenoidOnly));
    rt.enterSafeState();
    CHECK(rt.air().state() == AirState::EStopped);
    CHECK(rt.actuator()->state() == MotionState::EStopped);
}
