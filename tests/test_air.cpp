/*
 * tests/test_air.cpp — modular air blocks + AirSystem timeline + safety.
 * Covers Section 18 "Air": solenoid peak/hold, servovalve, flow servo, fan,
 * pump direct, multi-pump, tank, sensor absent/stale, timeout, overpressure,
 * panic, short note, rapid note change.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/AirEngine.h"

using namespace swc;

struct FakeAirSink : IAirSink {
    float src[MAX_PUMPS] = {0,0,0};
    bool  gateOpen = false;
    float gatePwm = -1, flow = -1, angle = -1;
    float sensorRaw = NAN;   // set to a number to simulate a present sensor
    void setSourceLevel(uint8_t i, float v) override { if (i < MAX_PUMPS) src[i] = v; }
    void setGateOpen(bool o) override { gateOpen = o; }
    void setGatePwm(float v) override { gatePwm = v; }
    void setFlow(float v) override { flow = v; }
    void setAngle(float v) override { angle = v; }
    float readSensorRaw() override { return sensorRaw; }
};

template <typename Blk>
static uint32_t pumpBlk(Blk& b, uint32_t t0, uint32_t ms) {
    uint32_t t = t0; for (uint32_t i=0;i<ms;++i){ t+=1; b.update(t);} return t;
}

TEST(solenoid_simple_open_timeout) {
    FakeAirSink s; AirConfig c; c.gate.type = AirGateType::SolenoidSimple; c.gate.openTimeoutMs = 100;
    SolenoidSimpleGate g; g.begin(c, &s);
    g.open(0); CHECK(s.gateOpen);
    pumpBlk(g, 0, 50); CHECK(s.gateOpen);
    pumpBlk(g, 50, 100); CHECK(!s.gateOpen);
    CHECK(g.fault() == FaultCode::ValveTimeout);
}

TEST(solenoid_pwm_peak_then_hold) {
    FakeAirSink s; AirConfig c;
    c.gate.type = AirGateType::SolenoidPwm; c.gate.peak01 = 1.0f; c.gate.peakMs = 40; c.gate.hold01 = 0.4f;
    SolenoidPwmGate g; g.begin(c, &s);
    g.open(0); CHECK_NEAR(s.gatePwm, 1.0f, 1e-3);
    pumpBlk(g, 0, 45); CHECK_NEAR(s.gatePwm, 0.4f, 1e-3);   // dropped to hold
    g.safeState(); CHECK_NEAR(s.gatePwm, 0.0f, 1e-3);       // panic close
}

TEST(solenoid_pwm_hold_clamped_to_peak) {
    FakeAirSink s; AirConfig c;
    c.gate.type = AirGateType::SolenoidPwm; c.gate.peak01 = 0.6f; c.gate.hold01 = 0.9f; // invalid
    SolenoidPwmGate g; g.begin(c, &s);
    CHECK(g.holdLevel() <= 0.6f + 1e-6f);                   // consistency enforced
}

TEST(servo_valve_open_close_delays_and_safe) {
    FakeAirSink s; AirConfig c;
    c.gate.type = AirGateType::ServoValve; c.gate.closed01 = 0.1f; c.gate.open01 = 0.9f;
    c.gate.openDelayMs = 30; c.gate.closeDelayMs = 20;
    ServoValveGate g; g.begin(c, &s); CHECK_NEAR(s.gatePwm, 0.1f, 1e-3);
    g.open(0); pumpBlk(g, 0, 10); CHECK(!g.isOpen());       // still within open delay
    pumpBlk(g, 10, 30); CHECK(g.isOpen()); CHECK_NEAR(s.gatePwm, 0.9f, 1e-3);
    g.safeState(); CHECK(!g.isOpen()); CHECK_NEAR(s.gatePwm, 0.1f, 1e-3); // truly closed
}

TEST(fan_source_ready_after_spinup) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::FanPwm; c.source.spinUpMs = 100; c.source.min01 = 0.2f;
    FanSource f; f.begin(c, &s);
    AirNoteRequest r; r.velocity = 100;
    f.prepare(r, 0); CHECK(!f.ready());
    pumpBlk(f, 0, 50); CHECK(!f.ready());
    pumpBlk(f, 50, 60); CHECK(f.ready());
    CHECK(s.src[0] > 0.0f);
}

TEST(pump_direct_cascade) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsDirect; c.source.pumpCount = 3; c.source.cascadeDelayMs = 100;
    c.source.min01 = 0.3f; c.source.max01 = 1.0f;
    PumpDirectSource p; p.begin(c, &s);
    AirNoteRequest r; r.velocity = 127;
    p.prepare(r, 0);
    CHECK(s.src[0] > 0.0f); CHECK_NEAR(s.src[1], 0.0f, 1e-3);   // staggered
    p.run(r, 250); CHECK(s.src[2] > 0.0f);
    CHECK(p.ready());
}

TEST(pump_tank_no_sensor_no_autostart) {
    FakeAirSink s; s.sensorRaw = NAN;   // sensor absent from boot
    AirConfig c; c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.sensor.type = AirSensorType::PressureAnalog; c.sensor.staleTimeoutMs = 20;
    PumpTankSource p; p.begin(c, &s);
    for (uint32_t t = 1; t < 40; ++t) p.update(t);   // past the absence timeout
    CHECK(!p.ready());
    CHECK(p.fault() == FaultCode::SensorMissing);
    CHECK_NEAR(s.src[0], 0.0f, 1e-3);   // pumps stay off, never auto-started
}

TEST(pump_tank_fills_and_stops) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.lowThresh = 40; c.source.highThresh = 80; c.source.safetyThresh = 120;
    c.source.tankPwm = false; c.source.refillTimeoutMs = 100000; c.source.minOffMs = 0;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin = 0; c.sensor.rawMax = 100; c.sensor.physMin = 0; c.sensor.physMax = 100;
    c.sensor.physHi = 200;
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = 20;                    // low → should fill
    for (uint32_t t = 1; t < 30; ++t) { s.sensorRaw += 0.6f; p.update(t); }
    CHECK(s.src[0] > 0.0f);              // pump running while filling
    s.sensorRaw = 95;                    // above high → let EMA settle then stop
    for (uint32_t t = 200; t < 240; ++t) p.update(t);
    CHECK_NEAR(s.src[0], 0.0f, 1e-3);
    CHECK(p.ready());
}

// Review #4 §P0: the tank must not start pumping on the sensor's initial value
// (0) — it waits for the FIRST valid measurement (present=false until then).
TEST(pump_tank_waits_for_first_valid_measurement) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.lowThresh = 40; c.source.highThresh = 80; c.source.safetyThresh = 200;
    c.source.tankPwm = false; c.source.minOffMs = 0; c.source.refillTimeoutMs = 100000;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin = 0; c.sensor.rawMax = 100; c.sensor.physMin = 0; c.sensor.physMax = 100; c.sensor.physHi = 200;
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = NAN;                     // no valid reading yet
    for (uint32_t t = 1; t <= 5; ++t) p.update(t);
    CHECK_NEAR(s.src[0], 0.0f, 1e-3);      // pumps OFF — never regulate on the initial 0
    CHECK(!p.ready());
    s.sensorRaw = 20;                      // first valid measurement, below low
    for (uint32_t t = 6; t <= 12; ++t) p.update(t);
    CHECK(s.src[0] > 0.0f);                // now it may fill
}

TEST(pump_tank_overpressure) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.safetyThresh = 100;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin = 0; c.sensor.rawMax = 200; c.sensor.physMin = 0; c.sensor.physMax = 200; c.sensor.physHi = 300;
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = 150;                    // 150 phys > safety 100
    p.update(10); p.update(20);
    CHECK(p.fault() == FaultCode::Overpressure);
    CHECK_NEAR(s.src[0], 0.0f, 1e-3);
}

TEST(pump_tank_refill_timeout_rearmed) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.lowThresh = 40; c.source.highThresh = 80; c.source.safetyThresh = 200;
    c.source.refillTimeoutMs = 50; c.source.minOffMs = 0; c.source.tankPwm = false;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin = 0; c.sensor.rawMax = 100; c.sensor.physMin = 0; c.sensor.physMax = 100; c.sensor.physHi = 300;
    c.sensor.staleTimeoutMs = 100000;    // don't flag stale in this test
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = 10;                     // never reaches high → fill times out
    for (uint32_t t = 1; t < 80; ++t) p.update(t);
    CHECK(p.fault() == FaultCode::PumpTimeout);
}

TEST(flow_controller_minmax_and_curve) {
    FakeAirSink s; AirConfig c;
    c.flow.type = FlowControlType::FlowServo; c.flow.min = 10; c.flow.nominal = 200; c.flow.max = 100;
    c.flow.curve = VelocityCurve::Quadratic; c.flow.maxSlewPerMs = 1.0f; c.flow.rest01 = 0.0f;
    FlowController f; f.begin(c, &s);
    f.setTarget(100);                     // == max → span 1 → quad 1
    pumpBlk(f, 0, 5); CHECK_NEAR(f.current01(), 1.0f, 1e-2);
    f.setTarget(55);                      // mid span (55-10)/(100-10)=0.5 → 0.25
    pumpBlk(f, 5, 5); CHECK_NEAR(f.current01(), 0.25f, 2e-2);
    f.rest(); pumpBlk(f, 10, 5); CHECK_NEAR(f.current01(), 0.0f, 1e-2);
}

// --- AirSystem timeline -----------------------------------------------------
static AirConfig solenoidFlowPreset() {   // preset #2 flavour
    AirConfig c;
    c.source.type = AirSourceType::ExternalPassive;
    c.gate.type   = AirGateType::SolenoidSimple; c.gate.activeHigh = true;
    c.flow.type   = FlowControlType::FlowServo; c.flow.min = 0; c.flow.nominal = 64; c.flow.max = 127;
    c.flow.maxSlewPerMs = 1.0f;
    return c;
}

TEST(airsystem_note_timeline) {
    FakeAirSink s; AirSystem a; a.begin(solenoidFlowPreset(), &s);
    AirNoteRequest r; r.velocity = 100; r.airNominal = 80;
    a.setNow(0); a.prepareNote(r); CHECK(a.state() == AirState::Preparing);
    a.setNow(1); a.startNote(r);   CHECK(a.state() == AirState::Playing); CHECK(s.gateOpen);
    a.update(2); CHECK(s.flow > 0.0f);
    a.setNow(3); a.stopNote();     CHECK(!s.gateOpen);
    a.update(4); CHECK(a.state() == AirState::Idle);
}

TEST(airsystem_short_note_no_air) {
    // Note off before startNote → gate must never open (correction #1/#9 at air level)
    FakeAirSink s; AirSystem a; a.begin(solenoidFlowPreset(), &s);
    AirNoteRequest r; r.velocity = 100;
    a.setNow(0); a.prepareNote(r);
    a.setNow(1); a.stopNote();          // released during positioning
    a.update(2);
    CHECK(!s.gateOpen);                 // air never produced
}

TEST(airsystem_panic_blocks_reopen) {
    FakeAirSink s; AirSystem a; a.begin(solenoidFlowPreset(), &s);
    AirNoteRequest r; r.velocity = 100;
    a.setNow(0); a.startNote(r); CHECK(s.gateOpen);
    a.emergencyStop();
    CHECK(!s.gateOpen); CHECK(a.state() == AirState::EStopped);
    a.setNow(1); a.startNote(r);        // deferred command must NOT reopen air
    CHECK(!s.gateOpen);
    CHECK(!a.isReady());
}

TEST(airsystem_valve_timeout_safety) {
    FakeAirSink s; auto c = solenoidFlowPreset(); c.valveOpenTimeoutMs = 100;
    AirSystem a; a.begin(c, &s);
    AirNoteRequest r; r.velocity = 100;
    a.setNow(0); a.startNote(r); CHECK(s.gateOpen);
    for (uint32_t t = 1; t <= 150; ++t) a.update(t);
    CHECK(!s.gateOpen);
    CHECK(a.fault() == FaultCode::ValveTimeout);
    CHECK(a.state() == AirState::Fault);
}

// Review #14: a perfectly stable sensor reading is NOT stale.
TEST(sensor_stable_value_not_stale) {
    FakeAirSink s; s.sensorRaw = 50;
    AirConfig c; c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin=0; c.sensor.rawMax=100; c.sensor.physMin=0; c.sensor.physMax=100; c.sensor.physHi=200;
    c.sensor.staleTimeoutMs = 100;
    AirSensor sensor; sensor.begin(c, &s);
    for (uint32_t t = 1; t < 500; ++t) sensor.update(t);   // constant reading, long time
    CHECK(sensor.present());
    CHECK(sensor.valid());                                  // stable ≠ stale
    CHECK(sensor.fault() == FaultCode::None);
}

// Review #15: a transient NaN does not make the sensor permanently absent.
TEST(sensor_recovers_from_transient_nan) {
    FakeAirSink s; s.sensorRaw = 50;
    AirConfig c; c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin=0; c.sensor.rawMax=100; c.sensor.physMin=0; c.sensor.physMax=100; c.sensor.physHi=200;
    c.sensor.staleTimeoutMs = 100;
    AirSensor sensor; sensor.begin(c, &s);
    for (uint32_t t = 1; t < 20; ++t) sensor.update(t);
    CHECK(sensor.present());
    s.sensorRaw = NAN;                                      // brief glitch
    for (uint32_t t = 20; t < 40; ++t) sensor.update(t);    // < staleTimeout
    CHECK(sensor.present());                                // still considered present
    s.sensorRaw = 55;                                       // recovers
    for (uint32_t t = 40; t < 60; ++t) sensor.update(t);
    CHECK(sensor.present());
    CHECK(sensor.valid());
}

// Review #16: direct-pump cascade actually starts pumps 2/3 via update().
TEST(pump_direct_cascade_via_update) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsDirect; c.source.pumpCount = 3; c.source.cascadeDelayMs = 100;
    c.source.min01 = 0.3f; c.source.max01 = 1.0f;
    PumpDirectSource p; p.begin(c, &s);
    AirNoteRequest r; r.velocity = 127;
    p.prepare(r, 0);
    CHECK(s.src[0] > 0.0f); CHECK_NEAR(s.src[1], 0.0f, 1e-3);   // only pump 0 at t=0
    for (uint32_t t = 1; t < 250; ++t) p.update(t);            // advance with NO new note
    CHECK(s.src[1] > 0.0f); CHECK(s.src[2] > 0.0f);
    CHECK(p.ready());
}

// Review #18: an out-of-range sensor reading stops the tank pumps.
TEST(pump_tank_out_of_range_stops) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.lowThresh=40; c.source.highThresh=80; c.source.safetyThresh=200; c.source.minOffMs=0;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin=0; c.sensor.rawMax=100; c.sensor.physMin=0; c.sensor.physMax=100;
    c.sensor.physLo=0; c.sensor.physHi=90;   // valid band ends at 90
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = 99;                          // 99 > physHi 90 → out of range
    for (uint32_t t = 1; t < 10; ++t) p.update(t);
    CHECK(p.fault() == FaultCode::SensorOutOfRange);
    CHECK_NEAR(s.src[0], 0.0f, 1e-3);
}

// Review #13: air rearm clears a latched subcomponent (valve timeout) fault.
TEST(air_rearm_clears_subcomponent_fault) {
    FakeAirSink s; auto cfg = []{ AirConfig c; c.source.type=AirSourceType::ExternalPassive;
        c.gate.type=AirGateType::SolenoidSimple; c.valveOpenTimeoutMs=50; return c; }();
    AirSystem a; a.begin(cfg, &s);
    AirNoteRequest r; r.velocity=100;
    a.setNow(0); a.startNote(r);
    for (uint32_t t=1;t<=80;++t) a.update(t);
    CHECK(a.fault() == FaultCode::ValveTimeout);
    a.rearm();
    CHECK(a.fault() == FaultCode::None);
    CHECK(a.state() == AirState::Idle);
    a.setNow(100); a.startNote(r); a.update(101);   // usable again
    CHECK(s.gateOpen);
}

// Review #17: tank PWM regulation drives harder far from target than near it.
TEST(pump_tank_pid_targets_setpoint) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.lowThresh = 40; c.source.highThresh = 80; c.source.safetyThresh = 200;
    c.source.target = 70; c.source.tankPwm = true; c.source.pidKp = 0.02f; c.source.pidKi = 0.0f;
    c.source.min01 = 0.0f; c.source.max01 = 1.0f; c.source.minOffMs = 0; c.source.refillTimeoutMs = 1000000;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin=0; c.sensor.rawMax=100; c.sensor.physMin=0; c.sensor.physMax=100; c.sensor.physHi=200;
    c.sensor.filterAlpha = 1.0f;   // no smoothing lag for the test
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = 30;                          // far below target → strong drive, starts filling
    for (uint32_t t = 1; t < 5; ++t) p.update(t);
    float driveFar = s.src[0];
    s.sensorRaw = 66;                          // near target, still filling (<high)
    for (uint32_t t = 5; t < 10; ++t) p.update(t);
    float driveNear = s.src[0];
    CHECK(driveFar > driveNear);               // PI uses the setpoint
    CHECK(driveNear > 0.0f);
}

// Review #3 §10.1: the PI integral accumulates per unit TIME, not per call. Two
// runs with the SAME number of update() calls but different elapsed time must
// build different integral terms (old per-call code gave identical results).
TEST(pump_tank_pi_integral_is_time_scaled) {
    auto run = [](uint32_t step) {
        FakeAirSink s; AirConfig c;
        c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
        c.source.lowThresh = 65; c.source.highThresh = 80; c.source.safetyThresh = 200;
        c.source.target = 70; c.source.tankPwm = true;
        c.source.pidKp = 0.0f; c.source.pidKi = 0.1f;      // pure integral
        c.source.min01 = 0.0f; c.source.max01 = 1.0f;
        c.source.minOffMs = 0; c.source.refillTimeoutMs = 1000000;
        c.sensor.type = AirSensorType::PressureAnalog;
        c.sensor.rawMin=0; c.sensor.rawMax=100; c.sensor.physMin=0; c.sensor.physMax=100; c.sensor.physHi=200;
        c.sensor.filterAlpha = 1.0f;
        PumpTankSource p; p.begin(c, &s);
        s.sensorRaw = 60;                                   // constant err = 70-60 = 10, keeps filling
        uint32_t t = 100; p.update(t);                     // prime the dt clock (dt=0)
        for (int k = 0; k < 10; ++k) { t += step; p.update(t); }
        return s.src[0];
    };
    float driveFast = run(1);    // 10 calls, 1 ms apart  → 10 ms elapsed
    float driveSlow = run(10);   // 10 calls, 10 ms apart → 100 ms elapsed
    CHECK(driveSlow > driveFast * 3.0f);   // more time ⇒ more integral, per-call would tie
    CHECK(driveFast > 0.0f);
}

// Review #3 §10.2: a multi-pump tank stages pumps in over cascadeDelayMs from
// the fill start, rather than energising all motors at once.
TEST(pump_tank_multi_pump_cascade) {
    FakeAirSink s; AirConfig c;
    c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.source.lowThresh = 65; c.source.highThresh = 90; c.source.safetyThresh = 200;
    c.source.tankPwm = false;               // drive = max01 while filling
    c.source.pumpCount = 3; c.source.cascadeDelayMs = 10;
    c.source.minOffMs = 0; c.source.refillTimeoutMs = 1000000;
    c.sensor.type = AirSensorType::PressureAnalog;
    c.sensor.rawMin=0; c.sensor.rawMax=100; c.sensor.physMin=0; c.sensor.physMax=100; c.sensor.physHi=200;
    c.sensor.filterAlpha = 1.0f;
    PumpTankSource p; p.begin(c, &s);
    s.sensorRaw = 60;                        // below low → fills, stays filling (<high)
    p.update(1);                             // fill starts at t=1
    for (uint32_t t = 2; t <= 6; ++t) p.update(t);
    CHECK(s.src[0] > 0.0f);                  // pump 0 on immediately
    CHECK_NEAR(s.src[1], 0.0f, 1e-3);        // pump 1 still staged out (<10 ms)
    CHECK_NEAR(s.src[2], 0.0f, 1e-3);
    for (uint32_t t = 7; t <= 30; ++t) p.update(t);
    CHECK(s.src[1] > 0.0f);                  // pump 1 in after ~10 ms
    CHECK(s.src[2] > 0.0f);                  // pump 2 in after ~20 ms
}
