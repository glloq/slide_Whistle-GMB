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
    // Live config source for ApplyDynamicConfig (indexed by Instrument::id()).
    void setLiveConfig(const RuntimeConfig* cfg) { live_ = cfg; }

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
        }
    }

    void panicAll(uint32_t nowMs) {
        if (q_) q_->clear();
        for (uint8_t i = 0; i < MAX_INSTRUMENTS; ++i) testAir_[i].phase = TestAirPhase::Idle;  // cancel all tests
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

    // Apply the global transpose, clamped to a valid MIDI note.
    uint8_t transposed(uint8_t note) const {
        int n = int(note) + transpose_;
        if (n < 0) n = 0; else if (n > 127) n = 127;
        return uint8_t(n);
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
                uint8_t note = transposed(c.a);
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel) && inst_[i]->acceptsNote(note))
                        inst_[i]->noteOn(note, c.b, nowMs);
                break;
            }
            case CommandType::NoteOff: {
                uint8_t note = transposed(c.a);
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel))
                        inst_[i]->noteOff(note, nowMs);
                break;
            }
            case CommandType::ControlChange:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel)) {
                        if (c.a == 0xFF) inst_[i]->aftertouch(c.b, nowMs);
                        else             inst_[i]->controlChange(c.a, c.b, nowMs);
                    }
                break;
            case CommandType::PitchBend: {
                float semis = (float(c.i16) / 8192.0f) * bendRangeSemis_;
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel))
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
                if (in && in->actuator()) { in->actuator()->clearFault(); in->actuator()->requestHoming(); ack(c, ExecResult::Accepted); }
                else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::Rearm: {
                // Acknowledge fault + re-arm actuator & air, then re-home so the
                // instrument is usable again after panic (review #9).
                Instrument* in = byId(c.instrument);
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
                if (in && in->actuator()) {
                    bool ok = in->actuator()->requestPositionMm(in->actuator()->currentPositionMm() + float(c.i16));
                    ack(c, ok ? ExecResult::Accepted : ExecResult::Rejected);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::TestActuator: {
                // absolute test position (mm) in `a`; single-instrument only (#13)
                Instrument* in = byId(c.instrument);
                if (in && in->actuator()) {
                    bool ok = in->actuator()->requestPositionMm(float(c.a));
                    ack(c, ok ? ExecResult::Accepted : ExecResult::Rejected);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::TestAir: {
                // Start the non-blocking test only if the air system exists AND
                // is not faulted/e-stopped — otherwise the request would be
                // ignored yet reported Accepted (review #4 §P0).
                Instrument* in = byId(c.instrument);
                uint8_t slot = in ? in->id() : 0xFF;
                if (in && in->air() && in->air()->fault() == FaultCode::None && slot < MAX_INSTRUMENTS) {
                    TestAirState& ts = testAir_[slot];
                    ts.req = AirNoteRequest{}; ts.req.velocity = c.b ? c.b : 100;
                    ts.durMs = c.i16 > 0 ? (uint32_t)c.i16 : 3000u;
                    ts.startMs = nowMs;
                    ts.phase = TestAirPhase::Prepare;
                    in->air()->prepareNote(ts.req);
                    ack(c, ExecResult::Accepted);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::ApplyDynamicConfig:
                // Push the newly-saved dynamic parameters into the live objects
                // so the API's "applied" claim is truthful (correction #17).
                // Config is indexed by the instrument's stable id().
                if (live_) {
                    transpose_ = live_->midi.transpose;   // global transpose is dynamic (#5 §12)
                    for (uint8_t i = 0; i < count_; ++i)
                        if (inst_[i] && inst_[i]->id() < live_->instrumentCount)
                            inst_[i]->applyDynamic(live_->instruments[inst_[i]->id()]);
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
    const RuntimeConfig* live_ = nullptr;       // for ApplyDynamicConfig
    TestAirState       testAir_[MAX_INSTRUMENTS];                   // per-instrument TestAir FSM
    ExecAck            lastAck_{};                                  // last direct-command ack
    bool               blocked_ = false;                           // global-fault command gate
    std::atomic<bool>  safeRestartDone_{false};                    // RT reached safe state for reboot
};

} // namespace swc

#endif // SWC_CORE_REALTIMEENGINE_H
