/*
 * core/RealtimeEngine.h — the owner of all hardware objects (RT core).
 *
 * Drains the command queue, routes MIDI events to the matching instruments,
 * dispatches direct (non-MIDI) commands to a single instrument by index, and
 * ticks actuators / air / sequencers on a deterministic period.
 *
 * Direct test/home/jog commands carry an explicit instrument index so a
 * "test this flute" action can never reach another instrument through MIDI
 * routing (correction #13).
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_REALTIMEENGINE_H
#define SWC_CORE_REALTIMEENGINE_H

#include "Instrument.h"
#include "CommandQueue.h"
#include "RuntimeConfig.h"
#include "ConfigHandoff.h"
#include <atomic>

namespace swc {

// Execution acknowledgement for a direct command (Home/Jog/Test/Rearm). The API
// returns only "queued"; this lets a client confirm the RT task actually acted
// on the command — in particular that it was NOT silently dropped for an unknown
// instrument id (review #3 §7).
enum class ExecResult : uint8_t { None = 0, Accepted, Rejected };
struct ExecAck {
    uint32_t    seq = 0;
    CommandType type = CommandType::Panic;
    ExecResult  result = ExecResult::None;
};

template <uint16_t QN>
class RealtimeEngine {
public:
    void begin(Instrument** instruments, uint8_t count, CommandQueue<QN>* queue) {
        inst_ = instruments; count_ = count; q_ = queue;
        bendRangeSemis_ = 2.0f;
    }
    void setPitchBendRange(float semis) { bendRangeSemis_ = semis; }
    // Global MIDI transpose (semitones). Applied to NoteOn/NoteOff at the single
    // convergence point for every source, since the real path is the command
    // queue, not MidiRouter (review #5 §12). MainApp sets it from config.midi.
    void setTranspose(int8_t semis) { transpose_ = semis; }
    // Cross-core config source for ApplyDynamicConfig. The RT task copies the
    // published desired config out of the handoff (seqlock) into applied_ before
    // pushing it into the live objects — never reads a struct another core is
    // mid-writing (review #9 §4.2/§4.3). Indexed by Instrument::id().
    void setConfigSource(const ConfigHandoff* h) { handoff_ = h; }
    // Prime the applied generation at boot: the instruments are built directly
    // from generation `g` (InstrumentRuntime), so the objects already hold it
    // even though no ApplyDynamicConfig ran. Without this, telemetry would report
    // desired>applied at boot though they match.
    void primeAppliedGen(uint32_t g) { appliedGen_ = g; }
    // Generation currently reflected in the live objects (0 until the first
    // apply/prime). Telemetry compares it against the handoff's desiredGen() so a
    // client can tell a saved dynamic change has actually landed on the RT core.
    uint32_t appliedGen() const { return appliedGen_; }

    // Drain up to `budget` commands, then tick every instrument. Called from a
    // periodic RT task (vTaskDelayUntil) — no blocking, no allocation.
    void tick(uint32_t nowMs, uint32_t nowUs, uint16_t budget = 32) {
        Command c;
        while (budget-- && q_ && q_->pop(c)) dispatch(c, nowMs);

        // Drive the per-instrument TestAir state machine (review #4 §P0): the
        // test opens the gate only AFTER the source reports ready, holds for the
        // requested duration, then closes — never a blind immediate open.
        for (uint8_t i = 0; i < count_; ++i) {
            Instrument* in = inst_[i];
            if (!in) continue;
            uint8_t slot = in->id();
            if (slot < MAX_INSTRUMENTS) driveTestAir(slot, in->air(), nowMs);
        }

        for (uint8_t i = 0; i < count_; ++i) {
            Instrument* in = inst_[i];
            if (!in) continue;
            if (in->actuator()) in->actuator()->update(nowUs);
            if (in->air())      in->air()->update(nowMs);
            in->update(nowMs, nowUs);
            // Clear an instrument's diagnostic lock once the diagnostic has
            // actually finished — the axis is no longer homing/moving and no
            // TestAir is running — so musical events resume (review #8 §8).
            uint8_t slot = in->id();
            if (slot < MAX_INSTRUMENTS && diag_[slot]) {
                bool moving = in->actuator() &&
                    (in->actuator()->state() == MotionState::Moving ||
                     in->actuator()->state() == MotionState::Homing);
                bool testing = testAir_[slot].phase != TestAirPhase::Idle;
                if (!moving && !testing) diag_[slot] = false;
            }
        }
    }

    void panicAll(uint32_t nowMs) {
        if (q_) q_->clear();
        for (uint8_t i = 0; i < MAX_INSTRUMENTS; ++i) { testAir_[i].phase = TestAirPhase::Idle; diag_[i] = false; }  // cancel tests + diag locks
        for (uint8_t i = 0; i < count_; ++i) if (inst_[i]) inst_[i]->panic(nowMs);
    }

    bool testAirActive() const {
        for (uint8_t i = 0; i < MAX_INSTRUMENTS; ++i) if (testAir_[i].phase != TestAirPhase::Idle) return true;
        return false;
    }

    // Last direct command the RT task acted on (or rejected). A client that
    // enqueued a command with a given seq can confirm execution by matching it.
    const ExecAck& lastExec() const { return lastAck_; }

    // Global fault gate (review #4 §P0). When set, the engine refuses every
    // actuation command (NoteOn/CC/PitchBend/Jog/TestActuator/TestAir); only
    // safety/recovery commands (Panic, NoteOff, Home, Rearm, SafeRestart,
    // config/calibration) still pass — so the announced Fault state and the real
    // behaviour agree. MainApp drives this from the system state.
    void setCommandsBlocked(bool b) { blocked_ = b; }
    bool commandsBlocked() const { return blocked_; }

    // Set once the RT task has brought everything to a safe state in response to
    // a SafeRestart command; the (network) task polls this before rebooting so
    // the reboot is never issued while the RT task still owns the actuators.
    // Atomic because it is written on the RT core and read on the network core
    // (review #5 §P0.5).
    bool safeRestartDone() const { return safeRestartDone_.load(std::memory_order_acquire); }

private:
    void ack(const Command& c, ExecResult r) { lastAck_ = ExecAck{ c.seq, c.type, r }; }

    // Apply the global transpose; return -1 when the result falls outside the
    // valid MIDI range so the caller drops the note instead of clamping it.
    int transposedOrDrop(uint8_t note) const {
        int n = int(note) + transpose_;
        return (n < 0 || n > 127) ? -1 : n;
    }

    // Non-blocking TestAir state machine, one per instrument.
    enum class TestAirPhase : uint8_t { Idle = 0, Prepare, WaitReady, Hold };
    struct TestAirState {
        TestAirPhase   phase = TestAirPhase::Idle;
        AirNoteRequest req;
        uint32_t       startMs = 0;   // when Prepare/WaitReady began (ready timeout)
        uint32_t       holdMs = 0;    // when Hold (gate open) began
        uint32_t       durMs = 3000;
    };
    static constexpr uint32_t kTestReadyTimeoutMs = 5000;   // give up if source never readies

    void driveTestAir(uint8_t slot, IAirSystem* air, uint32_t nowMs) {
        TestAirState& ts = testAir_[slot];
        if (ts.phase == TestAirPhase::Idle) return;
        // Abort the test if the air system faults mid-sequence.
        if (!air || air->fault() != FaultCode::None) {
            if (air) air->stopNote();
            ts.phase = TestAirPhase::Idle;
            return;
        }
        switch (ts.phase) {
            case TestAirPhase::Prepare:
                // prepareNote() was issued on dispatch; move to waiting for ready.
                ts.startMs = nowMs;
                ts.phase = TestAirPhase::WaitReady;
                break;
            case TestAirPhase::WaitReady:
                if (air->isReady()) {                       // source spun up / tank ready
                    air->startNote(ts.req);
                    ts.holdMs = nowMs;
                    ts.phase = TestAirPhase::Hold;
                } else if (elapsed_u32(nowMs, ts.startMs) > kTestReadyTimeoutMs) {
                    air->stopNote();                         // never opened; give up
                    ts.phase = TestAirPhase::Idle;
                }
                break;
            case TestAirPhase::Hold:
                if (elapsed_u32(nowMs, ts.holdMs) >= ts.durMs) {
                    air->stopNote();
                    ts.phase = TestAirPhase::Idle;
                }
                break;
            default: break;
        }
    }

    // Actuation commands that must be refused while a global fault is latched.
    static bool isActuation(CommandType t) {
        return t == CommandType::NoteOn || t == CommandType::ControlChange ||
               t == CommandType::PitchBend || t == CommandType::Jog ||
               t == CommandType::TestActuator || t == CommandType::TestAir;
    }

    // Resolve a direct command's target by the instrument's STABLE id(), so
    // commands keep addressing the same physical flute even when the live set
    // is compacted (disabled instruments removed) — array index would drift.
    Instrument* byId(uint8_t id) {
        for (uint8_t i = 0; i < count_; ++i)
            if (inst_[i] && inst_[i]->id() == id) return inst_[i];
        return nullptr;
    }

    // Diagnostics (Home/Jog/TestActuator/TestAir) are EXCLUSIVE: they must not
    // disturb an active musical note or run concurrently with another test on
    // the same instrument (review #6 §12). A busy target rejects the command.
    bool instrumentBusy(Instrument* in) {
        if (!in) return false;
        if (in->sequencer().activeNoteOr(-1) >= 0 || in->sequencer().heldCount() > 0) return true;
        uint8_t slot = in->id();
        if (slot < MAX_INSTRUMENTS && testAir_[slot].phase != TestAirPhase::Idle) return true;
        // A diagnostic must also wait for any in-flight motion or air activity —
        // otherwise two commands in the same drain batch (Home+Jog, Jog+TestActuator,
        // TestActuator+TestAir) overwrite each other's target (review #7 §5).
        if (auto* a = in->actuator())
            if (a->state() == MotionState::Moving || a->state() == MotionState::Homing) return true;
        if (auto* air = in->air()) {
            AirState s = air->state();
            if (s == AirState::Preparing || s == AirState::Playing || s == AirState::Releasing) return true;
        }
        return false;
    }

    // Re-arm only makes sense when something is actually faulted/e-stopped.
    // Without this guard a Rearm sent from Ready during a note would clear the
    // air and force a re-home mid-play (review #7 §5).
    // A musical event (NoteOn/CC/PitchBend) must be refused while the instrument
    // is running a diagnostic, mirroring the diagnostic-vs-note exclusion the
    // other way round (review #8 §8). Keyed by stable id().
    bool inDiagnostic(Instrument* in) const {
        if (!in) return false;
        uint8_t slot = in->id();
        return slot < MAX_INSTRUMENTS && diag_[slot];
    }
    void markDiagnostic(Instrument* in) {
        if (!in) return;
        uint8_t slot = in->id();
        if (slot < MAX_INSTRUMENTS) diag_[slot] = true;
    }

    bool instrumentNeedsRearm(Instrument* in) {
        if (!in) return false;
        if (auto* a = in->actuator())
            if (a->state() == MotionState::Fault || a->state() == MotionState::EStopped ||
                a->fault() != FaultCode::None) return true;
        if (auto* air = in->air())
            if (air->state() == AirState::Fault || air->state() == AirState::EStopped ||
                air->fault() != FaultCode::None) return true;
        return false;
    }

    void dispatch(const Command& c, uint32_t nowMs) {
        // Global fault: refuse actuation, but let safety/recovery through so the
        // system can still be stopped and re-armed (review #4 §P0).
        if (blocked_ && isActuation(c.type)) {
            if (c.type != CommandType::NoteOn && c.type != CommandType::ControlChange &&
                c.type != CommandType::PitchBend)
                ack(c, ExecResult::Rejected);   // direct commands get a rejection ack
            return;
        }
        switch (c.type) {
            case CommandType::NoteOn: {
                // A note transposed out of 0..127 is DROPPED, not clamped — a
                // clamp would merge several source notes onto 0/127 (#6 §13).
                int note = transposedOrDrop(c.a);
                if (note < 0) break;
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && !inDiagnostic(inst_[i]) &&
                        inst_[i]->acceptsChannel(c.channel) && inst_[i]->acceptsNote(uint8_t(note)))
                        inst_[i]->noteOn(uint8_t(note), c.b, nowMs);
                break;
            }
            case CommandType::NoteOff: {
                int note = transposedOrDrop(c.a);
                if (note < 0) break;
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel))
                        inst_[i]->noteOff(uint8_t(note), nowMs);
                break;
            }
            case CommandType::ControlChange:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && !inDiagnostic(inst_[i]) && inst_[i]->acceptsChannel(c.channel)) {
                        if (c.a == 0xFF) inst_[i]->aftertouch(c.b, nowMs);
                        else             inst_[i]->controlChange(c.a, c.b, nowMs);
                    }
                break;
            case CommandType::PitchBend: {
                float semis = (float(c.i16) / 8192.0f) * bendRangeSemis_;
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && !inDiagnostic(inst_[i]) && inst_[i]->acceptsChannel(c.channel))
                        inst_[i]->pitchBend(semis, nowMs);
                break;
            }
            case CommandType::Panic:
                panicAll(nowMs);
                break;
            case CommandType::SafeRestart:
                // The RT task (sole owner of the actuators) brings everything to
                // a safe state; the network task waits for safeRestartDone() and
                // only then reboots — no cross-core actuator access (review #4 §P0).
                // Refuse any further actuation once a restart is in flight (§P0.5).
                blocked_ = true;
                panicAll(nowMs);
                safeRestartDone_.store(true, std::memory_order_release);
                ack(c, ExecResult::Accepted);
                break;
            case CommandType::Home: {
                // Direct commands address instruments by STABLE id(), not the
                // compact array index — otherwise a "home flute 2" reaches the
                // wrong physical flute once disabled ones are compacted (#3 §6).
                Instrument* in = byId(c.instrument);
                if (in && instrumentBusy(in)) { ack(c, ExecResult::Rejected); break; }
                if (in && in->actuator()) { in->actuator()->clearFault(); in->actuator()->requestHoming(); markDiagnostic(in); ack(c, ExecResult::Accepted); }
                else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::Rearm: {
                // Acknowledge fault + re-arm actuator & air, then re-home so the
                // instrument is usable again after panic (review #9).
                Instrument* in = byId(c.instrument);
                if (in && !instrumentNeedsRearm(in)) { ack(c, ExecResult::Rejected); break; }
                if (in) {
                    if (in->actuator()) in->actuator()->clearFault();
                    if (in->air())      in->air()->rearm();
                    if (in->actuator()) in->actuator()->requestHoming();
                    ack(c, ExecResult::Accepted);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::Jog: {
                // Jog is a SIGNED relative move (mm) carried in i16 — a uint8_t
                // absolute could never express a negative delta (#3 §7.4). Report
                // the actuator's real accept/reject, not a blind Accepted — a
                // move refused for soft-limit/not-homed/fault must NOT ack
                // Accepted (review #5 §9).
                Instrument* in = byId(c.instrument);
                if (in && instrumentBusy(in)) { ack(c, ExecResult::Rejected); break; }
                if (in && in->actuator()) {
                    bool ok = in->actuator()->requestPositionMm(in->actuator()->currentPositionMm() + float(c.i16));
                    if (ok) markDiagnostic(in);
                    ack(c, ok ? ExecResult::Accepted : ExecResult::Rejected);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::TestActuator: {
                // absolute test position (mm) in `a`; single-instrument only (#13)
                Instrument* in = byId(c.instrument);
                if (in && instrumentBusy(in)) { ack(c, ExecResult::Rejected); break; }
                if (in && in->actuator()) {
                    bool ok = in->actuator()->requestPositionMm(float(c.a));
                    if (ok) markDiagnostic(in);
                    ack(c, ok ? ExecResult::Accepted : ExecResult::Rejected);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::TestAir: {
                // Start the non-blocking test only if the air system exists AND
                // is not faulted/e-stopped — otherwise the request would be
                // ignored yet reported Accepted (review #4 §P0). Diagnostics are
                // exclusive: a busy instrument (note active) rejects it (#6 §12).
                Instrument* in = byId(c.instrument);
                uint8_t slot = in ? in->id() : 0xFF;
                if (in && instrumentBusy(in)) { ack(c, ExecResult::Rejected); break; }
                if (in && in->air() && in->air()->fault() == FaultCode::None && slot < MAX_INSTRUMENTS) {
                    TestAirState& ts = testAir_[slot];
                    ts.req = AirNoteRequest{}; ts.req.velocity = c.b ? c.b : 100;
                    ts.durMs = c.i16 > 0 ? (uint32_t)c.i16 : 3000u;
                    ts.startMs = nowMs;
                    ts.phase = TestAirPhase::Prepare;
                    in->air()->prepareNote(ts.req);
                    markDiagnostic(in);
                    ack(c, ExecResult::Accepted);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::ApplyDynamicConfig:
                // Copy the newly-saved config OUT of the cross-core handoff into
                // our own applied_ buffer (seqlock: whole-old or whole-new, never
                // a torn mix, review #9 §4.2/§4.3), then push the dynamic
                // parameters into the live objects so the API's "applied" claim is
                // truthful (correction #17). Indexed by the instrument's stable
                // id(). A load() of 0 means nothing published / copy unverified —
                // leave the objects and the applied generation untouched.
                if (handoff_) {
                    uint32_t g = handoff_->load(applied_);
                    if (g) {
                        // A transpose change would otherwise leave notes whose ON
                        // was transposed by the old amount unmatchable by their OFF
                        // (a stuck note) — release everything first (#6 §13).
                        if (applied_.midi.transpose != transpose_)
                            for (uint8_t i = 0; i < count_; ++i)
                                if (inst_[i]) inst_[i]->allNotesOff(nowMs);
                        transpose_ = applied_.midi.transpose;   // global transpose is dynamic (#5 §12)
                        for (uint8_t i = 0; i < count_; ++i)
                            if (inst_[i] && inst_[i]->id() < applied_.instrumentCount)
                                inst_[i]->applyDynamic(applied_.instruments[inst_[i]->id()]);
                        appliedGen_ = g;   // the objects now reflect this generation
                    }
                }
                break;
            default: break;   // calibration handled elsewhere
        }
    }

    Instrument**       inst_ = nullptr;
    uint8_t            count_ = 0;
    CommandQueue<QN>*  q_ = nullptr;
    float              bendRangeSemis_ = 2.0f;
    int8_t             transpose_ = 0;
    const ConfigHandoff* handoff_ = nullptr;    // cross-core config source (#4.2/#4.3)
    RuntimeConfig      applied_{};              // RT-private copy of the applied config
    uint32_t           appliedGen_ = 0;         // generation now in the live objects
    TestAirState       testAir_[MAX_INSTRUMENTS];                   // per-instrument TestAir FSM
    bool               diag_[MAX_INSTRUMENTS] = {false};           // per-instrument diagnostic lock (#8 §8)
    ExecAck            lastAck_{};                                  // last direct-command ack
    // Blocked by default: the RT task drains the queue on its very first tick,
    // before MainApp's lifecycle code sets the gate. Starting unblocked let a
    // command enqueued at boot (before homing) execute in that first drain
    // (review #7 §4). MainApp clears it only once the system reaches Ready.
    bool               blocked_ = true;                            // command gate (blocked until Ready)
    std::atomic<bool>  safeRestartDone_{false};                    // RT reached safe state for reboot
};

} // namespace swc

#endif // SWC_CORE_REALTIMEENGINE_H
