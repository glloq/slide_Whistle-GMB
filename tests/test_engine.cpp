/*
 * tests/test_engine.cpp — Instrument + MidiRouter + RealtimeEngine.
 * Covers convergence of MIDI inputs, channel/range routing, CC mapping
 * (correction #9), test-endpoint scoping (correction #13), and queue→engine.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/RealtimeEngine.h"
#include "../esp32/esp32_slide_whistle/core/MidiRouter.h"
#include "../esp32/esp32_slide_whistle/core/AirEngine.h"
#include "../esp32/esp32_slide_whistle/core/SlideActuators.h"

using namespace swc;

struct FASink : IAirSink {
    bool gateOpen = false; float flow = 0, angle = -1;
    void setSourceLevel(uint8_t, float) override {}
    void setGateOpen(bool o) override { gateOpen = o; }
    void setGatePwm(float) override {}
    void setFlow(float v) override { flow = v; }
    void setAngle(float v) override { angle = v; }
    float readSensorRaw() override { return NAN; }
};

static AirConfig air2() {
    AirConfig c; c.source.type = AirSourceType::ExternalPassive;
    c.gate.type = AirGateType::SolenoidSimple; c.flow.type = FlowControlType::FlowServo;
    c.flow.maxSlewPerMs = 1.0f; return c;
}

// One playable instrument built on a Disabled actuator (always ready).
struct InstRig {
    DisabledSlideActuator act;
    AirSystem air; FASink sink; NoteMap map;
    Instrument inst;
    void begin(uint8_t id, const InstrumentConfig& cfg) {
        SlideMotionConfig mc; mc.type = SlideDriveType::Disabled; act.begin(mc);
        air.begin(air2(), &sink);
        for (int n = 48; n <= 84; ++n) map.setPoint(uint8_t(n), (n-48)*2.0f, 60);
        inst.begin(id, &act, &air, &map, cfg);
    }
};

static InstrumentConfig icfg(uint8_t ch, uint8_t lo, uint8_t hi) {
    InstrumentConfig c; c.enabled = true; c.midiChannel = ch; c.noteMin = lo; c.noteMax = hi;
    return c;
}

TEST(engine_routes_by_channel) {
    InstRig r1, r2;
    r1.begin(0, icfg(1, 48, 84));
    r2.begin(1, icfg(2, 48, 84));
    Instrument* insts[] = {&r1.inst, &r2.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 2, &q);
    MidiRouter<32> router(q);

    router.noteOn(1, 60, 100);          // channel 1 → only instrument 0
    for (int k = 0; k < 3; ++k) eng.tick(k, k*1000);
    CHECK(r1.sink.gateOpen);
    CHECK(!r2.sink.gateOpen);
}

TEST(engine_split_by_note_range) {
    InstRig low, high;
    low.begin(0, icfg(1, 48, 59));      // low split
    high.begin(1, icfg(1, 60, 84));     // high split, same channel
    Instrument* insts[] = {&low.inst, &high.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 2, &q);
    MidiRouter<32> router(q);
    router.noteOn(1, 55, 100);
    router.noteOn(1, 72, 100);
    for (int k = 0; k < 3; ++k) eng.tick(k, k*1000);
    CHECK(low.sink.gateOpen);
    CHECK(high.sink.gateOpen);
    CHECK_EQ(low.inst.sequencer().activeNoteOr(), 55);
    CHECK_EQ(high.inst.sequencer().activeNoteOr(), 72);
}

TEST(engine_transpose) {
    InstRig r; r.begin(0, icfg(1, 48, 84));
    Instrument* insts[] = {&r.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 1, &q);
    MidiRouter<32> router(q); router.setTranspose(12);
    router.noteOn(1, 48, 100);          // +12 → 60
    for (int k = 0; k < 3; ++k) eng.tick(k, k*1000);
    CHECK_EQ(r.inst.sequencer().activeNoteOr(), 60);
}

TEST(cc1_vibrato_only_when_enabled) {
    // With vibrato mapped to CC1 and enabled, CC1 drives vibrato.
    InstrumentConfig cfg = icfg(1, 48, 84);
    cfg.cc.vibrato = 1; cfg.cc.vibratoEnabled = true;
    InstRig r; r.begin(0, cfg);
    r.inst.noteOn(60, 100, 0);
    r.inst.controlChange(1, 100, 1);    // CC1 → vibrato active (no crash, applied)
    r.inst.update(2, 2000);
    CHECK(r.inst.enabled());

    // With vibrato DISABLED, CC1 must NOT drive vibrato (correction #9).
    InstrumentConfig cfg2 = icfg(1, 48, 84);
    cfg2.cc.vibrato = 1; cfg2.cc.vibratoEnabled = false;
    InstRig r2; r2.begin(0, cfg2);
    r2.inst.noteOn(60, 100, 0);
    // Route CC1 many times; position must stay put (no vibrato modulation).
    float before = r2.act.currentPositionMm();
    for (int k = 1; k < 20; ++k) { r2.inst.controlChange(1, 127, k); r2.inst.update(k, k*1000); }
    CHECK_NEAR(r2.act.currentPositionMm(), before, 1e-3);
}

TEST(cc_breath_maps_to_flow) {
    InstrumentConfig cfg = icfg(1, 48, 84);
    cfg.cc.breath = 2;
    InstRig r; r.begin(0, cfg);
    r.inst.noteOn(60, 100, 0); r.air.update(1); r.inst.update(1, 1000);
    r.inst.controlChange(2, 127, 2);    // breath → flow target high
    for (int k = 2; k < 8; ++k) { r.air.setNow(k); r.air.update(k); }
    CHECK(r.sink.flow > 0.5f);
}

TEST(test_endpoint_hits_one_instrument_only) {
    // Two instruments on the SAME MIDI channel; a TestActuator command with an
    // explicit index must move only that one (correction #13).
    InstRig r0, r1;
    // use real steppers so a target is observable via the sink
    // (Disabled ignores targets, so build stepper rigs here)
    FASink s0, s1;
    struct MSink : IMotionSink { float mm=0; void writeStepperMm(float v) override{mm=v;}
        void writeServoUs(uint8_t,uint16_t) override{} void enableDriver(bool) override{}
        bool readEndstop(bool) override{return mm<=0.0f;} } m0, m1;
    StepDirSlideActuator a0(&m0), a1(&m1);
    SlideMotionConfig mc; mc.type = SlideDriveType::StepDir; mc.travelMm=100; mc.softMaxMm=100;
    mc.maxSpeedMmS=200; mc.accelMmS2=2000; mc.stepper.stepsPerMm=80;
    mc.stepper.homingFastMmS=200; mc.stepper.phaseTimeoutMs=5000; mc.stepper.homeBackoffMm=2;
    a0.begin(mc); a1.begin(mc);
    AirSystem air0, air1; air0.begin(air2(), &s0); air1.begin(air2(), &s1);
    NoteMap map0, map1;
    Instrument i0, i1;
    i0.begin(0, &a0, &air0, &map0, icfg(1,48,84));
    i1.begin(1, &a1, &air1, &map1, icfg(1,48,84));
    a0.requestHoming(); a1.requestHoming();
    for (int k=0;k<2000 && !(a0.isHomed()&&a1.isHomed());++k){ a0.update(k*1000); a1.update(k*1000);}
    CHECK(a0.isHomed()); CHECK(a1.isHomed());

    Instrument* insts[] = {&i0, &i1};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 2, &q);
    Command t{CommandType::TestActuator}; t.instrument = 1; t.a = 40;   // move instrument 1 to 40 mm
    q.push(t);
    for (uint32_t k=3000; k<7000; ++k) eng.tick(k, k*1000);
    CHECK_NEAR(a1.currentPositionMm(), 40.0f, 1.0f);
    CHECK_NEAR(a0.currentPositionMm(), 0.0f, 1.0f);   // instrument 0 untouched
}

TEST(engine_pitchbend_via_queue) {
    InstRig r; r.begin(0, icfg(1, 48, 84));
    Instrument* insts[] = {&r.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 1, &q);
    eng.setPitchBendRange(2.0f);
    MidiRouter<32> router(q);
    router.noteOn(1, 60, 100);
    router.pitchBend(1, 4096);          // +0.5 of range → +1 semitone
    for (int k = 0; k < 5; ++k) eng.tick(k, k*1000);
    // note 60 at 2mm/semi from map: 60→24mm base, +1 semi → 26mm
    CHECK_NEAR(r.act.currentPositionMm(), 26.0f, 0.5);
}
