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
    int   syncCount = 0;          // times the executed-step counter was realigned
    float lastSyncMm = -1e9f;     // mm passed to the last syncPositionMm()
    void writeStepperMm(float mm) override { lastStepperMm = mm; }
    void writeServoUs(uint8_t i, uint16_t us) override { if (i < 2) servoUs[i] = us; }
    void enableDriver(bool on) override { driverOn = on; }
    bool readEndstop(bool) override { return lastStepperMm <= endstopAtMm; }
    void syncPositionMm(float mm) override { ++syncCount; lastSyncMm = mm; }
};

// Sink that models a step backend emitting a BOUNDED number of pulses per call,
// so the executed position trails the commanded one during a fast move — used to
// exercise air-gating on the executed position.
struct LagSink : IMotionSink {
    float commandedMm = 0; long curSteps = 0;
    float stepsPerMm = 80.0f; long maxPerCall = 8;
    void writeStepperMm(float mm) override {
        commandedMm = mm;
        long target = lroundf(mm * stepsPerMm);
        long delta = target - curSteps;
        long steps = delta >= 0 ? delta : -delta;
        if (steps > maxPerCall) steps = maxPerCall;
        curSteps += (delta > 0 ? steps : -steps);
    }
    void writeServoUs(uint8_t, uint16_t) override {}
    void enableDriver(bool) override {}
    bool readEndstop(bool) override { return commandedMm <= 0.0f; }   // homing only
    void syncPositionMm(float mm) override { curSteps = lroundf(mm * stepsPerMm); }
    bool executedPositionMm(float& mm) const override { mm = float(curSteps) / stepsPerMm; return true; }
};

// Sink with independently forceable min/max endstops (plus a homing-side trip),
// for continuous endstop supervision during play.
struct EndstopSink : IMotionSink {
    float commandedMm = 0; bool minTrig = false, maxTrig = false; bool driverOn = false;
    void writeStepperMm(float mm) override { commandedMm = mm; }
    void writeServoUs(uint8_t, uint16_t) override {}
    void enableDriver(bool on) override { driverOn = on; }
    bool readEndstop(bool useMax) override {
        if (useMax) return maxTrig;
        return minTrig || commandedMm <= 0.0f;   // min also trips at the home ref
    }
    void syncPositionMm(float) override {}
};

// Sink whose MAX endstop trips once the position rises to/above a threshold,
// for exercising homeTowardZero=false.
struct MaxHomeSink : IMotionSink {
    float lastStepperMm = 0;
    float maxTripMm = 1e9f;
    int   syncCount = 0;
    float lastSyncMm = -1e9f;
    void writeStepperMm(float mm) override { lastStepperMm = mm; }
    void writeServoUs(uint8_t, uint16_t) override {}
    void enableDriver(bool) override {}
    bool readEndstop(bool useMax) override { return useMax && lastStepperMm >= maxTripMm; }
    void syncPositionMm(float mm) override { ++syncCount; lastSyncMm = mm; }
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
    float p = 0;
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
    float p = 0;
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
    // …and it is a real latched fault now: state Fault, driver off, not ready
    // for air (review #5 §P0.3).
    CHECK(act.state() == MotionState::Fault);
    CHECK(!act.isReadyForAir());
    CHECK(!sink.driverOn);
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

// Review #6 §19: an e-stop triggered by an existing fault must preserve the root
// cause in fault(), not overwrite it with EmergencyStop.
TEST(estop_preserves_root_fault) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    act.begin(stepperCfg()); act.requestHoming(); pump(act, 0, 3000);
    CHECK(!act.requestPositionMm(150.0f));        // out of range → TargetOutOfRange
    CHECK(act.fault() == FaultCode::TargetOutOfRange);
    act.emergencyStop();
    CHECK(act.state() == MotionState::EStopped);
    CHECK(act.fault() == FaultCode::TargetOutOfRange);   // root cause kept
    // A clean actuator e-stopped still reports EmergencyStop.
    StepDirSlideActuator act2(&sink); act2.begin(stepperCfg());
    act2.emergencyStop();
    CHECK(act2.fault() == FaultCode::EmergencyStop);
}

TEST(disabled_actuator_always_ready) {
    DisabledSlideActuator act;
    SlideMotionConfig c; c.type = SlideDriveType::Disabled;
    CHECK(act.begin(c));
    CHECK(act.isHomed());
    CHECK(act.isReadyForAir());
    CHECK(act.requestPositionMm(999.0f));    // accepted no-op
}

// Review #1: rearm after an e-stop without reconstructing the object.
TEST(stepper_clearfault_rearm_and_rehome) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    act.begin(stepperCfg()); act.requestHoming(); pump(act, 0, 3000);
    CHECK(act.isHomed());
    act.emergencyStop();
    CHECK(act.state() == MotionState::EStopped);
    CHECK(!act.requestHoming());             // blocked while latched
    act.clearFault();                         // re-arm
    CHECK(act.state() == MotionState::Idle);
    CHECK(act.requestHoming());               // homing allowed again
    pump(act, 4000000, 3000);
    CHECK(act.isHomed());
    CHECK(act.fault() == FaultCode::None);
}

// Review #11: a distant endstop must not be clamped away by the soft limits,
// and the offset phase must actually move to the offset.
TEST(stepper_homing_distant_switch_and_offset) {
    FakeSink sink; sink.endstopAtMm = -30.0f;   // switch 30 mm below soft-min (0)
    auto c = stepperCfg();
    c.stepper.homeOffsetMm = 5.0f;               // park 5 mm off the switch
    c.stepper.phaseTimeoutMs = 5000;
    StepDirSlideActuator act(&sink);
    CHECK(act.begin(c));
    CHECK(act.requestHoming());
    pump(act, 0, 6000);                          // seek travels past soft-min, then offsets
    CHECK(act.isHomed());
    CHECK(act.fault() == FaultCode::None);
    CHECK_NEAR(act.currentPositionMm(), 5.0f, 0.6);   // real move to offset, not a snap
}

// Review #7 §1: at the precise homing contact the actuator must realign the
// executed-step counter to its new mm reference (0 at the min end), otherwise
// the first post-home move drives a phantom correction on real hardware.
TEST(stepper_homing_syncs_step_counter_at_min) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    CHECK(act.begin(stepperCfg()));
    CHECK(act.requestHoming());
    pump(act, 0, 3000);
    CHECK(act.isHomed());
    CHECK(sink.syncCount >= 1);              // counter was realigned at contact
    CHECK_NEAR(sink.lastSyncMm, 0.0f, 1e-4); // to the min reference (0 mm)
}

// Review #7 §1: homing toward the MAX end must define the contact as travelMm
// (not 0) and then move INWARD by the offset — so subsequent positive targets
// stay inside the course instead of driving back into the max butée.
TEST(stepper_homing_toward_max_reference) {
    MaxHomeSink sink; sink.maxTripMm = 100.0f;   // switch at the travel end
    auto c = stepperCfg();
    c.stepper.homeTowardZero = false;
    c.stepper.homeOffsetMm = 5.0f;
    c.stepper.phaseTimeoutMs = 6000;
    StepDirSlideActuator act(&sink);
    CHECK(act.begin(c));
    CHECK(act.requestHoming());
    pump(act, 0, 8000);
    CHECK(act.isHomed());
    CHECK(act.fault() == FaultCode::None);
    CHECK_NEAR(sink.lastSyncMm, 100.0f, 1e-4);          // reference defined at travelMm
    CHECK_NEAR(act.currentPositionMm(), 95.0f, 0.8);    // parked 5 mm inward, not at 0
    CHECK(act.requestPositionMm(20.0f));                // an inward target is accepted
}

// Review #8 §5: on a fast reversal (velocity still positive, new target to the
// left and close) the profile must DECELERATE the opposing velocity, never
// accelerate it further in the wrong direction.
TEST(stepper_reversal_does_not_accelerate_wrong_way) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    auto c = stepperCfg(); c.softMaxMm = 100; c.maxSpeedMmS = 200; c.accelMmS2 = 1000;
    StepDirSlideActuator act(&sink);
    act.begin(c); act.requestHoming(); pump(act, 0, 3000);
    // Build a rightward velocity toward 60 mm.
    CHECK(act.requestPositionMm(60.0f));
    uint32_t t = 4000;
    while (act.velocityMmS() < 80.0f && t < 4000 + 2000) { t += 1000; act.update(t); }
    CHECK(act.velocityMmS() > 0.0f);                 // moving right
    float vBefore = act.velocityMmS();
    float posAt = act.currentPositionMm();
    // Command a target just to the LEFT (close) — a reversal.
    CHECK(act.requestPositionMm(posAt - 0.5f));
    t += 1000; act.update(t);                         // one integration step
    // The (still positive) velocity must have DECREASED, not grown.
    CHECK(act.velocityMmS() < vBefore);
    // And over the next few steps it keeps dropping toward/through zero, it does
    // not run away toward vmax.
    float vPrev = act.velocityMmS();
    for (int i = 0; i < 5; ++i) { t += 1000; act.update(t); CHECK(act.velocityMmS() <= vPrev + 1e-3f); vPrev = act.velocityMmS(); }
}

// Review #7/#8 §6: air must be gated on the EXECUTED position. While the step
// backend is still emitting the last bounded batch of pulses, the commanded
// pos_ can already equal the target (state Holding) yet the slide has not
// physically arrived — isReadyForAir() must stay false until the executed
// position catches up.
TEST(stepper_air_gating_waits_for_executed_position) {
    LagSink sink; sink.stepsPerMm = 80; sink.maxPerCall = 8;   // ~0.1 mm/tick executed
    auto c = stepperCfg(); c.stepper.stepsPerMm = 80; c.maxSpeedMmS = 200; c.accelMmS2 = 2000;
    StepDirSlideActuator act(&sink);
    act.begin(c); act.requestHoming();
    uint32_t t = pump(act, 0, 3000);         // continuous clock from here on
    CHECK(act.isHomed());
    CHECK(act.requestPositionMm(60.0f));
    // Run until the profile has settled (pos == target, not moving).
    for (int i = 0; i < 4000 && act.isMoving(); ++i) { t += 1000; act.update(t); }
    CHECK_NEAR(act.currentPositionMm(), 60.0f, 0.5f);
    float exec; sink.executedPositionMm(exec);
    CHECK(exec < 60.0f - 1.0f);              // executed still trailing
    CHECK(!act.isReadyForAir());             // so air must NOT be allowed yet
    // Keep ticking (applyOutput keeps emitting pulses) until executed arrives.
    for (int i = 0; i < 4000 && !act.isReadyForAir(); ++i) { t += 1000; act.update(t); }
    sink.executedPositionMm(exec);
    CHECK_NEAR(exec, 60.0f, 0.2f);
    CHECK(act.isReadyForAir());              // now the slide has physically arrived
}

// Review #7/#8 §7: endstops are supervised continuously, not only during
// homing. An endstop that reads triggered mid-travel is inconsistent → the axis
// stops, faults EndstopInconsistent and de-energises the driver.
TEST(stepper_endstop_during_play_faults) {
    EndstopSink sink;
    auto c = stepperCfg(); c.stepper.endstopMin.pin = 34; c.stepper.homeBackoffMm = 3;
    StepDirSlideActuator act(&sink);
    act.begin(c); act.requestHoming();
    uint32_t t = pump(act, 0, 3000);
    CHECK(act.isHomed());
    CHECK(act.requestPositionMm(40.0f));
    t = pump(act, t, 2000);
    CHECK_NEAR(act.currentPositionMm(), 40.0f, 1.0f);
    CHECK(act.fault() == FaultCode::None);      // healthy mid-travel
    // Force the MIN endstop while parked at 40 mm — physically impossible → fault.
    sink.minTrig = true;
    t += 1000; act.update(t);
    CHECK(act.state() == MotionState::Fault);
    CHECK(act.fault() == FaultCode::EndstopInconsistent);
    CHECK(!sink.driverOn);                       // driver de-energised
    CHECK(!act.isReadyForAir());
}

// Review #6: applyDynamic changes speed/accel/soft-limits live (not pins/type).
TEST(actuator_apply_dynamic_soft_limits) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    auto c = stepperCfg(); c.softMaxMm = 50;
    act.begin(c); act.requestHoming(); pump(act, 0, 3000);
    CHECK(!act.requestPositionMm(80.0f));            // beyond old soft max
    // widen the soft limit dynamically → same target now accepted
    auto c2 = c; c2.softMaxMm = 100;
    act.clearFault();                                 // clear the TargetOutOfRange fault
    act.applyDynamic(c2);
    CHECK(act.requestPositionMm(80.0f));
}

// Review #7 §13: shrinking the soft-limit window around a position that would
// fall OUTSIDE it must not take effect live (it would clamp pos_ without a real
// move). The shrink is refused; the old window stays until a safe moment.
TEST(actuator_apply_dynamic_soft_limit_shrink_refused) {
    FakeSink sink; sink.endstopAtMm = 0.0f;
    StepDirSlideActuator act(&sink);
    auto c = stepperCfg(); c.softMaxMm = 100;
    act.begin(c); act.requestHoming(); pump(act, 0, 3000);
    CHECK(act.requestPositionMm(80.0f));              // move toward 80
    pump(act, 4000, 3000);
    CHECK_NEAR(act.currentPositionMm(), 80.0f, 1.0f); // settled at 80, idle
    // Now shrink the window to [0,50] — 80 is outside it. The shrink must be
    // refused so pos_ is not silently clamped to 50.
    auto c2 = c; c2.softMaxMm = 50;
    act.applyDynamic(c2);
    CHECK_NEAR(act.currentPositionMm(), 80.0f, 1.0f); // unchanged, not clamped to 50
    CHECK(act.requestPositionMm(70.0f));              // old window still in force
    // A shrink that still contains the current position IS applied.
    auto c3 = c; c3.softMaxMm = 90;
    pump(act, 8000, 3000);                            // reach 70 and settle idle
    act.applyDynamic(c3);
    CHECK(!act.requestPositionMm(95.0f));             // 95 now beyond the new max
}
