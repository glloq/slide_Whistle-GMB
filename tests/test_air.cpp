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
    FakeAirSink s; s.sensorRaw = NAN;   // sensor absent
    AirConfig c; c.source.type = AirSourceType::PumpsTank; c.source.requireSensor = true;
    c.sensor.type = AirSensorType::PressureAnalog;
    PumpTankSource p; p.begin(c, &s);
    p.update(10);
    CHECK(!p.ready());
    CHECK(p.fault() == FaultCode::SensorMissing);
    CHECK_NEAR(s.src[0], 0.0f, 1e-3);   // pumps stay off
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
