/*
 * tests/test_motion.cpp — NoteMap + slide actuator behaviour.
 * Covers Section 18 "Déplacement" and "conversion note→mm / interpolation /
 * pitch bend / vibrato / limites / homing timeout / servo inversion / dual".
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/NoteMap.h"
#include "../esp32/esp32_slide_whistle/core/SlideActuators.h"

using namespace swc;

// Recording fake hardware sink with a programmable endstop.
struct FakeSink : IMotionSink {
    float lastStepperMm = 0;
    uint16_t servoUs[2] = {0, 0};
    bool driverOn = false;
    // endstop trips once the commanded stepper position passes this point.
    float endstopAtMm = -1000.0f;
    void writeStepperMm(float mm) override { lastStepperMm = mm; }
    void writeServoUs(uint8_t i, uint16_t us) override { if (i < 2) servoUs[i] = us; }
    void enableDriver(bool on) override { driverOn = on; }
    bool readEndstop(bool) override { return lastStepperMm <= endstopAtMm; }
};

static SlideMotionConfig stepperCfg() {
    SlideMotionConfig c;
    c.type = SlideDriveType::StepDir;
    c.travelMm = 100; c.softMinMm = 0; c.softMaxMm = 100;
    c.maxSpeedMmS = 200; c.accelMmS2 = 2000;
    c.stepper.stepsPerMm = 80;
    c.stepper.homingFastMmS = 50; c.stepper.homingSlowMmS = 5;
    c.stepper.homeTowardZero = true; c.stepper.homeBackoffMm = 3;
    c.stepper.homeOffsetMm = 0; c.stepper.phaseTimeoutMs = 2000;
    return c;
}

// drive update() forward for `ms` in 1 kHz steps starting at t0 (us)
template <typename A>
static uint32_t pump(A& act, uint32_t t0Us, uint32_t ms) {
    uint32_t t = t0Us;
    for (uint32_t i = 0; i < ms; ++i) { t += 1000; act.update(t); }
    return t;
}

TEST(notemap_interpolation_and_pitchbend) {
    NoteMap m;
    m.setTravelMm(100);
    m.setPoint(60, 20.0f);
    m.setPoint(72, 80.0f);   // one octave, non-linear real world but linear here
    float p;
    CHECK(m.positionForNote(60, p)); CHECK_NEAR(p, 20.0f, 1e-3);
    CHECK(m.positionForNote(72, p)); CHECK_NEAR(p, 80.0f, 1e-3);
    // fractional note interpolates between calibrated neighbours (not fixed mm/semi)
    CHECK(m.positionForNote(66, p)); CHECK_NEAR(p, 50.0f, 1e-3);
    // pitch bend +0.5 semitone from 60
    CHECK(m.positionForNote(60.5f, p)); CHECK_NEAR(p, 22.5f, 1e-3);
    // below/above calibration clamps
    CHECK(m.positionForNote(40, p)); CHECK_NEAR(p, 20.0f, 1e-3);
    CHECK(m.positionForNote(90, p)); CHECK_NEAR(p, 80.0f, 1e-3);
}

TEST(notemap_nonlinear_neighbours) {
    // Three calibrated points with unequal spacing → interpolation must use the
    // *nearest* calibrated neighbours, not a global linear fit.
    NoteMap m;
    m.setPoint(60, 10.0f);
    m.setPoint(64, 30.0f);
    m.setPoint(72, 90.0f);
    float p;
    CHECK(m.positionForNote(62, p)); CHECK_NEAR(p, 20.0f, 1e-3);   // between 60..64
    CHECK(m.positionForNote(68, p)); CHECK_NEAR(p, 60.0f, 1e-3);   // between 64..72
    CHECK(m.isMonotonic());
}

TEST(notemap_nonmonotonic_detected) {
    NoteMap m;
    m.setPoint(60, 50.0f);
    m.setPoint(62, 40.0f);   // goes backwards
    CHECK(!m.isMonotonic());
    CHECK_EQ(m.firstNonMonotonic(), 62);
}

TEST(notemap_vibrato_units) {
    NoteMap m;
    m.setPoint(60, 0.0f);
    m.setPoint(72, 120.0f);   // 10 mm per semitone locally
    CHECK_NEAR(m.vibratoSemitones(60, 50, VibratoUnit::Cents), 0.5f, 1e-3);
    CHECK_NEAR(m.vibratoSemitones(60, 1, VibratoUnit::Semitone), 1.0f, 1e-3);
    CHECK_NEAR(m.vibratoSemitones(60, 5, VibratoUnit::Millimetre), 0.5f, 1e-2);
}

TEST(stepper_homing_completes) {
    FakeSink sink;
    sink.endstopAtMm = 0.0f;      // trips at/below 0
    StepDirSlideActuator act(&sink);
    CHECK(act.begin(stepperCfg()));
    CHECK(!act.isHomed());
    // moving toward zero from 0 immediately trips → backoff → slow → offset
    CHECK(act.requestHoming());
    pump(act, 0, 3000);
    CHECK(act.isHomed());
    CHECK(act.state() == MotionState::Idle);
    CHECK(act.fault() == FaultCode::None);
}

TEST(stepper_homing_timeout_no_infinite_loop) {
    FakeSink sink;
    sink.endstopAtMm = -1e6f;     // endstop never trips
    StepDirSlideActuator act(&sink);
    auto c = stepperCfg(); c.stepper.phaseTimeoutMs = 200;
    CHECK(act.begin(c));
    CHECK(act.requestHoming());
    pump(act, 0, 500);            // returns — does NOT hang
    CHECK(!act.isHomed());
    CHECK(act.state() == MotionState::Fault);
    CHECK(act.fault() == FaultCode::HomingTimeout);
}

TEST(stepper_move_respects_limits) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    CHECK(act.begin(stepperCfg()));
    act.requestHoming(); pump(act, 0, 3000);
    CHECK(!act.requestPositionMm(150.0f));       // beyond soft max → rejected
    CHECK(act.fault() == FaultCode::TargetOutOfRange);
    // valid move
    StepDirSlideActuator act2(&sink);
    act2.begin(stepperCfg()); act2.requestHoming(); pump(act2, 0, 3000);
    CHECK(act2.requestPositionMm(50.0f));
    pump(act2, 3000000, 4000);
    CHECK_NEAR(act2.currentPositionMm(), 50.0f, 0.6);
    CHECK(act2.isReadyForAir());
}

TEST(stepper_not_ready_before_home) {
    FakeSink sink;
    StepDirSlideActuator act(&sink);
    act.begin(stepperCfg());
    CHECK(!act.isReadyForAir());
    CHECK(!act.requestPositionMm(10.0f));   // refuses target before homing
}

TEST(single_servo_inversion_and_ready) {
    FakeSink sink;
    SingleServoSlideActuator act(&sink);
    SlideMotionConfig c; c.type = SlideDriveType::SingleServo;
    c.travelMm = 100; c.softMaxMm = 100; c.maxSpeedMmS = 500; c.accelMmS2 = 5000;
    c.servoA.minUs = 1000; c.servoA.maxUs = 2000;
    c.servoA.cal[0] = {0, 1000}; c.servoA.cal[1] = {100, 2000}; c.servoA.calCount = 2;
    CHECK(act.begin(c));
    act.requestHoming(); pump(act, 0, 10);
    CHECK(act.isHomed());
    act.requestPositionMm(50.0f); pump(act, 100000, 2000);
    CHECK_NEAR(sink.servoUs[0], 1500, 20);
    // inverted servo maps the same mm to the mirrored pulse
    SingleServoSlideActuator inv(&sink);
    auto ci = c; ci.servoA.invert = true;
    inv.begin(ci); inv.requestHoming(); pump(inv, 0, 10);
    inv.requestPositionMm(0.0f); pump(inv, 200000, 500);
    CHECK_NEAR(sink.servoUs[0], 2000, 20);      // 0 mm → 1000 µs, inverted → 2000
}

TEST(dual_servo_opposite_same_cycle) {
    FakeSink sink;
    DualServoSlideActuator act(&sink);
    SlideMotionConfig c; c.type = SlideDriveType::DualServo;
    c.travelMm = 100; c.softMaxMm = 100; c.maxSpeedMmS = 500; c.accelMmS2 = 5000;
    c.dualMode = DualSyncMode::Opposite; c.servoBEnabled = true;
    c.servoA.cal[0] = {0, 1000}; c.servoA.cal[1] = {100, 2000}; c.servoA.calCount = 2;
    c.servoB.cal[0] = {0, 1000}; c.servoB.cal[1] = {100, 2000}; c.servoB.calCount = 2;
    CHECK(act.begin(c));
    act.requestHoming(); pump(act, 0, 10);
    act.requestPositionMm(25.0f); pump(act, 100000, 2000);
    // A at 25 mm → 1250 µs ; B opposite (75 mm) → 1750 µs, both written
    CHECK_NEAR(sink.servoUs[0], 1250, 25);
    CHECK_NEAR(sink.servoUs[1], 1750, 25);
    CHECK(act.openLoop());
}

TEST(emergency_stop_disables_and_blocks) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    act.begin(stepperCfg()); act.requestHoming(); pump(act, 0, 3000);
    act.emergencyStop();
    CHECK(act.state() == MotionState::EStopped);
    CHECK(!sink.driverOn);
    CHECK(!act.requestPositionMm(10.0f));    // no motion after e-stop
}

TEST(disabled_actuator_always_ready) {
    DisabledSlideActuator act;
    SlideMotionConfig c; c.type = SlideDriveType::Disabled;
    CHECK(act.begin(c));
    CHECK(act.isHomed());
    CHECK(act.isReadyForAir());
    CHECK(act.requestPositionMm(999.0f));    // accepted no-op
}
