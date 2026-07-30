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

// Review #16: TestAir must stop itself server-side (never wait on the browser).
TEST(engine_testair_auto_timeout) {
    InstRig r; r.begin(0, icfg(1, 48, 84));
    Instrument* insts[] = {&r.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 1, &q);
    Command t{CommandType::TestAir}; t.instrument = 0; t.b = 100; t.i16 = 50;  // 50 ms
    q.push(t);
    uint32_t k = 0;
    eng.tick(k, k * 1000); k++;             // dispatch → air opens
    CHECK(r.sink.gateOpen);
    CHECK(eng.testAirActive());
    for (; k < 80; ++k) eng.tick(k, k * 1000);   // past 50 ms
    CHECK(!r.sink.gateOpen);                // auto-closed
}

// Review #17: a dynamic config change (ApplyDynamicConfig) actually reaches the
// live objects — here a re-calibrated note table changes the commanded mm.
TEST(engine_apply_dynamic_updates_live_notemap) {
    // stepper so we can observe the commanded position
    struct MSink2 : IMotionSink { float mm=0; void writeStepperMm(float v) override{mm=v;}
        void writeServoUs(uint8_t,uint16_t) override{} void enableDriver(bool) override{}
        bool readEndstop(bool) override{return mm<=0.0f;} } m;
    StepDirSlideActuator act(&m);
    SlideMotionConfig mc; mc.type=SlideDriveType::StepDir; mc.travelMm=100; mc.softMaxMm=100;
    mc.maxSpeedMmS=400; mc.accelMmS2=4000; mc.stepper.stepsPerMm=80;
    mc.stepper.homingFastMmS=400; mc.stepper.phaseTimeoutMs=5000; mc.stepper.homeBackoffMm=2;
    act.begin(mc);
    AirSystem air; FASink sink; air.begin(air2(), &sink);
    NoteMap map; for (int n=48;n<=84;++n) map.setPoint((uint8_t)n,(n-48)*2.0f,60);  // 60→24mm
    Instrument inst; inst.begin(0, &act, &air, &map, icfg(1,48,84));
    act.requestHoming();
    uint32_t t=0; for (int k=0;k<3000 && !act.isHomed();++k){t++;act.update(t*1000);} CHECK(act.isHomed());

    Instrument* insts[]={&inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts,1,&q);

    // live config with a DIFFERENT calibration: note 60 → 50 mm
    RuntimeConfig live = defaultConfig(); live.instrumentCount=1;
    live.instruments[0]=icfg(1,48,84);
    for (int n=48;n<=84;++n) live.instruments[0].map.setPoint((uint8_t)n,(n-48)*5.0f,60); // 60→60mm
    eng.setLiveConfig(&live);

    Command a{CommandType::ApplyDynamicConfig}; q.push(a);
    for (uint32_t k=t; k<t+50; ++k) eng.tick(k, k*1000);   // apply
    // now play note 60 → should target the NEW 60 mm, not the old 24 mm
    Command on{CommandType::NoteOn}; on.channel=1; on.a=60; on.b=100; q.push(on);
    for (uint32_t k=t+50; k<t+3000; ++k) eng.tick(k, k*1000);
    CHECK_NEAR(act.currentPositionMm(), 60.0f, 1.0f);
}

// Review #9: a Rearm command recovers an instrument after panic (no reboot).
TEST(engine_rearm_after_panic) {
    InstRig r; r.begin(0, icfg(1, 48, 84));
    Instrument* insts[] = {&r.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 1, &q);
    q.push(Command{CommandType::Panic});
    eng.tick(0, 0);
    CHECK(r.air.state() == AirState::EStopped);
    // Rearm → air/actuator cleared, instrument usable again
    Command re{CommandType::Rearm}; re.instrument = 0; q.push(re);
    for (uint32_t k = 1; k < 20; ++k) eng.tick(k, k * 1000);
    CHECK(r.air.state() != AirState::EStopped);
    Command on{CommandType::NoteOn}; on.channel = 1; on.a = 60; on.b = 100; q.push(on);
    for (uint32_t k = 20; k < 25; ++k) eng.tick(k, k * 1000);
    CHECK(r.sink.gateOpen);                 // plays after rearm
}

// Review #11: two concurrent air tests each get their own timeout — the first
// is not orphaned when the second starts.
TEST(engine_two_testair_sessions_independent) {
    InstRig r0, r1;
    r0.begin(0, icfg(1, 48, 84));
    r1.begin(1, icfg(2, 48, 84));
    Instrument* insts[] = {&r0.inst, &r1.inst};
    CommandQueue<32> q; RealtimeEngine<32> eng; eng.begin(insts, 2, &q);
    Command t0{CommandType::TestAir}; t0.instrument = 0; t0.i16 = 40; q.push(t0);
    eng.tick(0, 0);
    Command t1{CommandType::TestAir}; t1.instrument = 1; t1.i16 = 40; q.push(t1);
    eng.tick(5, 5000);
    CHECK(r0.sink.gateOpen); CHECK(r1.sink.gateOpen);
    // advance past instrument 0's timeout (started at t=0, dur 40) but before 1's
    for (uint32_t k = 6; k <= 44; ++k) eng.tick(k, k * 1000);
    CHECK(!r0.sink.gateOpen);               // #0 auto-closed on its own timer
    CHECK(r1.sink.gateOpen);                // #1 still running
    for (uint32_t k = 45; k <= 60; ++k) eng.tick(k, k * 1000);
    CHECK(!r1.sink.gateOpen);               // #1 auto-closed
}

// Review #3 §2.2: a full queue must still admit safety commands. A NoteOff or
// Panic evicts the newest queued non-priority command rather than being dropped.
TEST(queue_panic_never_dropped_when_full) {
    CommandQueue<4> q;
    for (int i = 0; i < 4; ++i) {
        Command on{CommandType::NoteOn}; on.a = uint8_t(60 + i); on.b = 100;
        CHECK(q.push(on));
    }
    CHECK_EQ(q.size(), 4u);
    // Full of non-priority: a Panic must still get in (evicting a NoteOn).
    Command panic{CommandType::Panic};
    CHECK(q.push(panic));
    CHECK_EQ(q.size(), 4u);
    CHECK_EQ(q.dropped(), 1u);       // one NoteOn evicted
    // Panic jumped to the front — it pops first.
    Command out;
    CHECK(q.pop(out));
    CHECK(out.type == CommandType::Panic);
}

// A full queue of ONLY non-priority commands admits a NoteOff by eviction; a
// non-priority command on a full queue is dropped.
TEST(queue_noteoff_evicts_but_noteon_dropped_when_full) {
    CommandQueue<3> q;
    for (int i = 0; i < 3; ++i) {
        Command on{CommandType::NoteOn}; on.a = uint8_t(60 + i);
        CHECK(q.push(on));
    }
    // Another NoteOn is rejected outright.
    Command extra{CommandType::NoteOn}; extra.a = 72;
    CHECK(!q.push(extra));
    CHECK_EQ(q.dropped(), 1u);
    // A NoteOff is admitted (evicting the newest NoteOn).
    Command off{CommandType::NoteOff}; off.a = 60;
    CHECK(q.push(off));
    CHECK_EQ(q.dropped(), 2u);
    CHECK_EQ(q.size(), 3u);
    Command out;
    CHECK(q.pop(out));
    CHECK(out.type == CommandType::NoteOff);   // priority popped first
}

// A queue completely full of priority commands rejects a further priority
// command (a redundant Panic behind a Panic is harmless), and pcount bookkeeping
// stays consistent across pops so later non-priority pushes still fit.
TEST(queue_priority_full_rejects_and_pcount_recovers) {
    CommandQueue<2> q;
    CHECK(q.push(Command{CommandType::Panic}));
    CHECK(q.push(Command{CommandType::NoteOff}));
    CHECK_EQ(q.size(), 2u);
    // Full of priority — another priority command cannot be admitted.
    CHECK(!q.push(Command{CommandType::Panic}));
    CHECK_EQ(q.dropped(), 1u);
    // Drain, then a normal NoteOn fits again (pcount decremented on pop).
    Command out;
    CHECK(q.pop(out)); CHECK(q.pop(out));
    CHECK(q.empty());
    Command on{CommandType::NoteOn}; on.a = 60;
    CHECK(q.push(on));
    // And a priority command still fits alongside it.
    CHECK(q.push(Command{CommandType::NoteOff}));
    CHECK_EQ(q.size(), 2u);
}
