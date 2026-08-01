/*
 * tests/test_sequencer.cpp — NoteSequencer + CommandQueue.
 * Covers Section 18 "Séquenceur": note on/off, note-off during positioning,
 * mono replacement, return-to-previous, duplicates, sustain, stale note-off,
 * legato, panic, minimum duration, millis() rollover, plus queue priority.
 */
#include "test_framework.h"
#include "../esp32/esp32_slide_whistle/core/NoteSequencer.h"
#include "../esp32/esp32_slide_whistle/core/AirEngine.h"
#include "../esp32/esp32_slide_whistle/core/SlideActuators.h"
#include "../esp32/esp32_slide_whistle/core/CommandQueue.h"

using namespace swc;

struct FakeAirSink2 : IAirSink {
    bool gateOpen = false; float flow = 0, gatePwm = 0, angle = 0; float src = 0;
    int gateCloses = 0;   // count 1→0 transitions to catch spurious re-triggers
    void setSourceLevel(uint8_t, float v) override { src = v; }
    void setGateOpen(bool o) override { if (gateOpen && !o) ++gateCloses; gateOpen = o; }
    void setGatePwm(float v) override { gatePwm = v; }
    void setFlow(float v) override { flow = v; }
    void setAngle(float v) override { angle = v; }
    float readSensorRaw() override { return NAN; }
};
struct FakeMotionSink : IMotionSink {
    float mm = 0; bool endstop = false;
    void writeStepperMm(float v) override { mm = v; }
    void writeServoUs(uint8_t, uint16_t) override {}
    void enableDriver(bool) override {}
    bool readEndstop(bool) override { return endstop || mm <= 0.0f; }
};

static AirConfig simpleAir() {
    AirConfig c; c.source.type = AirSourceType::ExternalPassive;
    c.gate.type = AirGateType::SolenoidSimple; c.flow.type = FlowControlType::FlowServo;
    c.flow.maxSlewPerMs = 1.0f; return c;
}
static NoteMap makeMap() {
    NoteMap m; m.setTravelMm(100);
    for (int n = 48; n <= 84; ++n) m.setPoint(uint8_t(n), (n - 48) * 2.5f, 60);
    return m;
}

// Harness with a Disabled actuator (always ready) → air opens on next update.
struct SeqRig {
    DisabledSlideActuator act;
    AirSystem air; FakeAirSink2 sink; NoteMap map = makeMap();
    NoteSequencer seq;
    void begin(SequencerConfig cfg = {}) {
        SlideMotionConfig mc; mc.type = SlideDriveType::Disabled; act.begin(mc);
        air.begin(simpleAir(), &sink);
        seq.begin(&act, &air, &map, cfg);
    }
    void tick(uint32_t& t) { t += 1; act.update(t * 1000); air.setNow(t); air.update(t); seq.update(t, t * 1000); }
};

TEST(seq_basic_note_on_off) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    CHECK(r.seq.phase() == SeqPhase::Playing);
    CHECK(r.sink.gateOpen);
    r.seq.noteOff(60, t); r.tick(t);
    CHECK(!r.sink.gateOpen);
    CHECK(r.seq.phase() == SeqPhase::Idle || r.seq.phase() == SeqPhase::Releasing);
}

TEST(seq_mono_last_note_returns_to_previous) {
    // Mandatory case: NoteOn C, NoteOn E, NoteOff E → back to C.
    SeqRig r; SequencerConfig cfg; cfg.mono = MonoPolicy::LastNote; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);   // C
    r.seq.noteOn(64, 100, t); r.tick(t);   // E
    CHECK_EQ(r.seq.activeNoteOr(), 64);
    r.seq.noteOff(64, t); r.tick(t);
    CHECK_EQ(r.seq.activeNoteOr(), 60);    // back to C
    CHECK(r.sink.gateOpen);                // still playing C
}

TEST(seq_mono_highest_lowest) {
    { SeqRig r; SequencerConfig cfg; cfg.mono = MonoPolicy::HighestNote; r.begin(cfg);
      uint32_t t = 0;
      r.seq.noteOn(60, 100, t); r.tick(t);
      r.seq.noteOn(72, 100, t); r.tick(t);
      r.seq.noteOn(65, 100, t); r.tick(t);
      CHECK_EQ(r.seq.activeNoteOr(), 72); }
    { SeqRig r; SequencerConfig cfg; cfg.mono = MonoPolicy::LowestNote; r.begin(cfg);
      uint32_t t = 0;
      r.seq.noteOn(60, 100, t); r.tick(t);
      r.seq.noteOn(72, 100, t); r.tick(t);
      r.seq.noteOn(55, 100, t); r.tick(t);
      CHECK_EQ(r.seq.activeNoteOr(), 55); }
}

TEST(seq_duplicate_note_on) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    r.seq.noteOn(60, 120, t); r.tick(t);   // duplicate refresh, no extra held note
    CHECK_EQ(r.seq.heldCount(), 1);
    CHECK_EQ(r.seq.activeNoteOr(), 60);
}

TEST(seq_stale_note_off_ignored) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    r.seq.noteOff(48, t); r.tick(t);       // never pressed → ignore
    CHECK(r.sink.gateOpen);
    CHECK_EQ(r.seq.activeNoteOr(), 60);
}

TEST(seq_sustain_defers_release) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    r.seq.setSustain(true, t);
    r.seq.noteOff(60, t); r.tick(t);
    CHECK(r.sink.gateOpen);                 // held by pedal
    r.seq.setSustain(false, t); r.tick(t);
    CHECK(!r.sink.gateOpen);                // released on pedal up
}

TEST(seq_note_off_during_positioning_cancels_air) {
    // Real stepper: a move takes time; release before ready must cancel the
    // pending air open (correction #1). The gate must NEVER open.
    FakeMotionSink msink; StepDirSlideActuator act(&msink);
    SlideMotionConfig mc; mc.type = SlideDriveType::StepDir; mc.travelMm = 100;
    mc.softMaxMm = 100; mc.maxSpeedMmS = 20; mc.accelMmS2 = 100;
    mc.stepper.stepsPerMm = 80; mc.stepper.homingFastMmS = 100; mc.stepper.homingSlowMmS = 20;
    mc.stepper.homeBackoffMm = 2; mc.stepper.phaseTimeoutMs = 5000;
    act.begin(mc);
    AirSystem air; FakeAirSink2 sink; air.begin(simpleAir(), &sink);
    NoteMap map = makeMap(); NoteSequencer seq; seq.begin(&act, &air, &map, {});

    uint32_t t = 0;
    act.requestHoming();
    for (int i = 0; i < 2000 && !act.isHomed(); ++i) { t++; act.update(t * 1000); }
    CHECK(act.isHomed());

    seq.noteOn(84, 100, t);                 // far target → long move
    // advance a little: still Positioning, not ready, air not open
    for (int i = 0; i < 3; ++i) { t++; act.update(t*1000); air.setNow(t); air.update(t); seq.update(t, t*1000); }
    CHECK(seq.phase() == SeqPhase::Positioning);
    CHECK(!sink.gateOpen);
    seq.noteOff(84, t);                     // released before arrival
    for (int i = 0; i < 500; ++i) { t++; act.update(t*1000); air.setNow(t); air.update(t); seq.update(t, t*1000); }
    CHECK(!sink.gateOpen);                  // air was cancelled, never opened late
    CHECK(seq.phase() != SeqPhase::Playing);
}

TEST(seq_legato_glissando_keeps_air) {
    SeqRig r; SequencerConfig cfg; cfg.legato = LegatoPolicy::Glissando; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    CHECK(r.sink.gateOpen);
    r.sink.gateOpen = true;
    r.seq.noteOn(67, 100, t); r.tick(t);    // legato move: air stays open
    CHECK(r.sink.gateOpen);
}

// Review #4 §P1: releasing the top note while a lower one is still held must
// fall back to it and KEEP the air open under a legato-hold policy — not close
// it and leave a silent, still-"Playing" note.
TEST(seq_legato_release_fallback_not_silent) {
    SeqRig r; SequencerConfig cfg; cfg.mono = MonoPolicy::LastNote; cfg.legato = LegatoPolicy::Glissando; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t);
    for (int k = 0; k < 3; ++k) r.tick(t);
    CHECK(r.seq.phase() == SeqPhase::Playing);
    CHECK(r.sink.gateOpen);
    r.seq.noteOn(67, 100, t);                 // higher note, glissando keeps air
    for (int k = 0; k < 3; ++k) r.tick(t);
    CHECK(r.sink.gateOpen);
    r.seq.noteOff(67, t);                     // release top → fall back to 60
    for (int k = 0; k < 3; ++k) r.tick(t);
    CHECK(r.seq.phase() == SeqPhase::Playing);
    CHECK(r.sink.gateOpen);                    // must stay sounding, not go silent
    CHECK_EQ(r.seq.activeNoteOr(-1), 60);
}

// Review #5 §P0.3/§9: a note whose mapped position is outside the soft-limits
// is refused by the actuator; the sequencer must NOT open air and must abandon
// the note (the actuator also latches Fault).
TEST(seq_refuses_note_beyond_soft_limits) {
    FakeMotionSink m;
    StepDirSlideActuator act(&m);
    SlideMotionConfig mc; mc.type = SlideDriveType::StepDir; mc.travelMm = 100;
    mc.softMinMm = 0; mc.softMaxMm = 100; mc.maxSpeedMmS = 200; mc.accelMmS2 = 2000;
    mc.stepper.stepsPerMm = 80; mc.stepper.homingFastMmS = 200;
    mc.stepper.phaseTimeoutMs = 5000; mc.stepper.homeBackoffMm = 2;
    act.begin(mc);
    act.requestHoming();
    for (int k = 0; k < 3000 && !act.isHomed(); ++k) act.update(k * 1000);
    CHECK(act.isHomed());
    AirSystem air; FakeAirSink2 sink; air.begin(simpleAir(), &sink);
    NoteMap map; map.setTravelMm(100);
    for (int n = 48; n <= 84; ++n) map.setPoint((uint8_t)n, (n - 48) * 2.0f, 60);  // in range
    map.setPoint(72, 150.0f, 60);                                                  // beyond soft max
    NoteSequencer seq; seq.begin(&act, &air, &map, {});
    uint32_t t = 0;
    seq.noteOn(72, 100, t);
    for (int k = 0; k < 6; ++k) { t++; act.update(t * 1000); air.setNow(t); air.update(t); seq.update(t, t * 1000); }
    CHECK(!sink.gateOpen);                     // air never opened for the bad target
    CHECK(seq.phase() != SeqPhase::Playing);
    CHECK(act.state() == MotionState::Fault);  // actuator latched the fault
}

TEST(seq_legato_always_close_cuts_air) {
    SeqRig r; SequencerConfig cfg; cfg.legato = LegatoPolicy::AlwaysClose; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    // On the second note-on the air is explicitly closed before repositioning.
    bool closedDuringTransition = false;
    r.seq.noteOn(67, 100, t);
    if (!r.sink.gateOpen) closedDuringTransition = true;   // closed synchronously
    r.tick(t);
    CHECK(closedDuringTransition);
    CHECK(r.sink.gateOpen);                 // reopened for the new note
}

TEST(seq_panic_clears_everything) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    r.seq.setSustain(true, t);
    r.seq.panic(t);
    CHECK_EQ(r.seq.heldCount(), 0);
    CHECK_EQ(r.seq.activeNoteOr(), -1);
    CHECK(!r.sink.gateOpen);
    CHECK(r.air.state() == AirState::EStopped);
}

TEST(seq_min_note_duration) {
    SeqRig r; SequencerConfig cfg; cfg.minNoteMs = 50; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);    // t≈1
    r.seq.noteOff(60, t); r.tick(t);        // released almost immediately
    CHECK(r.sink.gateOpen);                 // held until min duration
    for (int i = 0; i < 60; ++i) r.tick(t);
    CHECK(!r.sink.gateOpen);                // released after minNoteMs
}

TEST(seq_millis_rollover) {
    SeqRig r; r.begin();
    uint32_t t = 0xFFFFFF00u;               // near wrap
    r.seq.noteOn(60, 100, t); r.tick(t);
    CHECK(r.sink.gateOpen);
    // cross the rollover boundary
    for (int i = 0; i < 400; ++i) r.tick(t);   // t wraps through 0
    CHECK(r.seq.phase() == SeqPhase::Playing); // still playing, no spurious release
    r.seq.noteOff(60, t); r.tick(t);
    CHECK(!r.sink.gateOpen);
}

// Review #5 §14: a min-note deferred release whose window straddles the millis()
// wrap fires at the correct time — not prematurely and not never.
TEST(seq_min_note_rollover_safe) {
    SeqRig r; SequencerConfig cfg; cfg.minNoteMs = 50; r.begin(cfg);
    uint32_t t = 0xFFFFFFE0u;                 // onset ~32 ms before the wrap
    r.seq.noteOn(60, 100, t); r.tick(t);
    r.seq.noteOff(60, t); r.tick(t);          // released early → deferred
    CHECK(r.sink.gateOpen);
    for (int i = 0; i < 30; ++i) r.tick(t);   // < 50 ms from onset, crossing the wrap
    CHECK(r.sink.gateOpen);                    // NOT released prematurely across wrap
    for (int i = 0; i < 40; ++i) r.tick(t);   // now > 50 ms from onset
    CHECK(!r.sink.gateOpen);                    // released at the right time
}

// Review #6 §17: a min-note deferred release belongs to the note that was
// active when it was requested. Pressing a NEW note before it fires must not let
// the stale release tear down the fresh note.
TEST(seq_min_note_deferred_release_superseded_by_new_note) {
    SeqRig r; SequencerConfig cfg; cfg.minNoteMs = 50; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);      // A pressed
    r.seq.noteOff(60, t); r.tick(t);          // A released early → deferred (guard)
    r.seq.noteOn(64, 100, t); r.tick(t);      // B pressed before the guard elapses
    CHECK_EQ(r.seq.activeNoteOr(), 64);
    CHECK(r.sink.gateOpen);
    int closesBefore = r.sink.gateCloses;
    for (int i = 0; i < 60; ++i) r.tick(t);   // past A's min-note window
    CHECK_EQ(r.seq.activeNoteOr(), 64);       // B still active, not torn down
    CHECK(r.sink.gateOpen);                    // and still sounding
    CHECK_EQ(r.sink.gateCloses, closesBefore); // A's stale guard never cut B's gate
}

// Review #6 §17: under sustain, releasing the active note defers it; pressing a
// new note supersedes it. Pedal-up must then NOT release the new note, which is
// still physically held.
TEST(seq_sustain_deferred_release_superseded_by_new_note) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);
    r.seq.setSustain(true, t);
    r.seq.noteOff(60, t); r.tick(t);          // A held by pedal (deferred)
    r.seq.noteOn(64, 100, t); r.tick(t);      // B pressed → supersedes A
    CHECK_EQ(r.seq.activeNoteOr(), 64);
    r.seq.setSustain(false, t);               // pedal up — no tick: observe immediately
    CHECK(r.sink.gateOpen);                    // B uninterrupted (stale defer would cut it)
    CHECK_EQ(r.seq.activeNoteOr(), 64);        // B still held → keeps sounding
}

// Review #5 §15: after a release completes (air idle, no note pending) the
// sequencer settles to Idle instead of lingering in Releasing.
TEST(seq_settles_to_idle_after_release) {
    SeqRig r; r.begin();
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t);
    for (int i = 0; i < 3; ++i) r.tick(t);
    CHECK(r.seq.phase() == SeqPhase::Playing);
    r.seq.noteOff(60, t);
    for (int i = 0; i < 10; ++i) r.tick(t);   // air closes + source idles
    CHECK(!r.sink.gateOpen);
    CHECK(r.seq.phase() == SeqPhase::Idle);    // settled, not stuck in Releasing
}

// --- CommandQueue -----------------------------------------------------------
TEST(queue_noteoff_and_panic_priority) {
    CommandQueue<8> q;
    Command on{CommandType::NoteOn}; on.a = 60;
    Command cc{CommandType::ControlChange}; cc.a = 11;
    Command off{CommandType::NoteOff}; off.a = 60;
    q.push(on); q.push(cc); q.push(off);    // off jumps ahead of on+cc
    Command out;
    CHECK(q.pop(out)); CHECK(out.type == CommandType::NoteOff);
    Command panic{CommandType::Panic};
    q.push(on); q.push(panic);
    CHECK(q.pop(out)); CHECK(out.type == CommandType::Panic);
}

TEST(queue_bounded_no_overflow) {
    CommandQueue<4> q;
    Command on{CommandType::NoteOn};
    for (int i = 0; i < 10; ++i) q.push(on);
    CHECK_EQ(q.size(), 4);
    CHECK(q.dropped() >= 6);
}

// Review #15: the sequencer must wait for BOTH actuator and air to be ready.
struct GateableAir : IAirSystem {
    bool ready_ = false; int started_ = 0; bool gate_ = false;
    bool begin(const AirConfig&, IAirSink*) override { return true; }
    void update(uint32_t) override {}
    void prepareNote(const AirNoteRequest&) override {}
    void startNote(const AirNoteRequest&) override { started_++; gate_ = true; }
    void updateExpression(const AirExpression&) override {}
    void stopNote() override { gate_ = false; }
    void emergencyStop() override { gate_ = false; ready_ = false; }
    void rearm() override { ready_ = false; }
    void applyDynamic(const AirConfig&) override {}
    bool isReady() const override { return ready_; }
    AirState state() const override { return gate_ ? AirState::Playing : AirState::Idle; }
    FaultCode fault() const override { return FaultCode::None; }
};

TEST(seq_waits_for_air_ready_before_opening) {
    DisabledSlideActuator act; SlideMotionConfig mc; mc.type = SlideDriveType::Disabled; act.begin(mc);
    GateableAir air; NoteMap map; for (int n = 48; n <= 84; ++n) map.setPoint((uint8_t)n, (n-48)*2.0f, 60);
    NoteSequencer seq; seq.begin(&act, &air, &map, {});
    uint32_t t = 0;
    seq.noteOn(60, 100, t);
    for (int k = 0; k < 5; ++k) { t++; seq.update(t, t*1000); }
    CHECK(seq.phase() == SeqPhase::Positioning);   // actuator ready, air not → wait
    CHECK_EQ(air.started_, 0);
    air.ready_ = true;
    t++; seq.update(t, t*1000);
    CHECK(seq.phase() == SeqPhase::Playing);        // air ready → opened
    CHECK_EQ(air.started_, 1);
}

TEST(seq_prepare_timeout_never_opens_late) {
    DisabledSlideActuator act; SlideMotionConfig mc; mc.type = SlideDriveType::Disabled; act.begin(mc);
    GateableAir air;                                 // air never becomes ready
    NoteMap map; for (int n = 48; n <= 84; ++n) map.setPoint((uint8_t)n, (n-48)*2.0f, 60);
    NoteSequencer seq; SequencerConfig cfg; cfg.prepareTimeoutMs = 50; seq.begin(&act, &air, &map, cfg);
    uint32_t t = 0;
    seq.noteOn(60, 100, t);
    for (int k = 0; k < 80; ++k) { t++; seq.update(t, t*1000); }
    CHECK(seq.phase() != SeqPhase::Playing);         // gave up
    CHECK_EQ(air.started_, 0);                        // air never opened late
}

// Review #9 §4.1: a note pressed while a modulation-wheel vibrato is already
// active must still start. The continuous LFO must not re-position the slide
// during Positioning (which kept flipping the actuator to Moving so
// isReadyForAir never became true and the note timed out without opening air).
TEST(seq_vibrato_active_before_note_still_opens) {
    FakeMotionSink m;                                  // readEndstop: mm <= 0
    StepDirSlideActuator act(&m);
    SlideMotionConfig mc; mc.type = SlideDriveType::StepDir;
    mc.travelMm = 100; mc.softMaxMm = 100; mc.maxSpeedMmS = 200; mc.accelMmS2 = 2000;
    mc.stepper.stepsPerMm = 80; mc.stepper.homingFastMmS = 200;
    mc.stepper.phaseTimeoutMs = 100000; mc.stepper.homeBackoffMm = 2;
    act.begin(mc);
    AirSystem air; FakeAirSink2 s; air.begin(simpleAir(), &s);
    NoteMap map; for (int n = 48; n <= 84; ++n) map.setPoint((uint8_t)n, (n-48)*2.0f, 60);
    SequencerConfig cfg; cfg.prepareTimeoutMs = 400; cfg.vibratoRateHz = 10.0f;
    NoteSequencer seq; seq.begin(&act, &air, &map, cfg);
    uint32_t us = 0;
    act.requestHoming();
    for (int k = 0; k < 3000 && !act.isHomed(); ++k) { us += 1000; act.update(us); }
    CHECK(act.isHomed());
    seq.setVibrato(50.0f, us / 1000);                  // ~0.5 semitone vibrato depth, before the note
    seq.noteOn(60, 100, us / 1000);
    for (int k = 0; k < 1500 && !s.gateOpen; ++k) {
        us += 1000; act.update(us); air.setNow(us / 1000); air.update(us / 1000); seq.update(us / 1000, us);
    }
    CHECK(s.gateOpen);                                 // the note opened despite active vibrato
    CHECK(seq.phase() == SeqPhase::Playing);
}

// Review #3 §5.1: a note with no usable position mapping must never be played —
// the sequencer must not enter Positioning/Playing or open air, and the note is
// dropped from the stack rather than left dangling.
TEST(seq_refuses_note_without_mapping) {
    DisabledSlideActuator act; SlideMotionConfig mc; mc.type = SlideDriveType::Disabled; act.begin(mc);
    AirSystem air; FakeAirSink2 sink; air.begin(simpleAir(), &sink);
    NoteMap map;                                      // empty: positionForNote always false
    NoteSequencer seq; seq.begin(&act, &air, &map, {});
    uint32_t t = 0;
    seq.noteOn(60, 100, t);
    for (int k = 0; k < 5; ++k) { t++; act.update(t*1000); air.setNow(t); air.update(t); seq.update(t, t*1000); }
    CHECK(seq.phase() != SeqPhase::Playing);          // never plays an unmapped note
    CHECK(seq.phase() != SeqPhase::Positioning);      // and does not wait to open air
    CHECK(!sink.gateOpen);                            // air never opened
    CHECK_EQ(seq.heldCount(), 0);                     // unmapped note dropped from the stack
}

// Review #9 §4.5: watchdogMs was validated and serialized but never enforced —
// a lost NoteOff (stuck key / unplugged cable) held the air open forever. The
// sequencer's per-note watchdog must force everything off once a single note has
// sounded longer than maxNoteMs, and clear on the next fresh key.
TEST(seq_note_watchdog_releases_stuck_note) {
    SeqRig r; SequencerConfig cfg; cfg.maxNoteMs = 20; r.begin(cfg);
    uint32_t t = 0;
    r.seq.noteOn(60, 100, t); r.tick(t);              // → Playing, air open
    CHECK(r.seq.phase() == SeqPhase::Playing);
    CHECK(r.sink.gateOpen);
    // A few ticks BEFORE the window: still sounding, watchdog quiet.
    for (int i = 0; i < 5; ++i) r.tick(t);
    CHECK(!r.seq.watchdogTripped());
    CHECK(r.sink.gateOpen);
    // No NoteOff ever arrives. Tick well past the watchdog window.
    for (int i = 0; i < 30; ++i) r.tick(t);
    CHECK(r.seq.watchdogTripped());                   // watchdog fired
    CHECK(!r.sink.gateOpen);                          // air forced closed
    CHECK(r.seq.phase() != SeqPhase::Playing);
    CHECK_EQ(r.seq.heldCount(), 0);                   // the stuck note was cleared
    // A fresh key clears the trip and plays normally again.
    r.seq.noteOn(62, 100, t); r.tick(t);
    CHECK(!r.seq.watchdogTripped());
    CHECK(r.sink.gateOpen);
}
