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
    // Live config source for ApplyDynamicConfig (indexed by Instrument::id()).
    void setLiveConfig(const RuntimeConfig* cfg) { live_ = cfg; }

    // Drain up to `budget` commands, then tick every instrument. Called from a
    // periodic RT task (vTaskDelayUntil) — no blocking, no allocation.
    void tick(uint32_t nowMs, uint32_t nowUs, uint16_t budget = 32) {
        Command c;
        while (budget-- && q_ && q_->pop(c)) dispatch(c, nowMs);

        // Server-side test-air timeout, PER INSTRUMENT so a second test never
        // orphans the first (reviews #16 + #11). Keyed by stable id() to match
        // the dispatch above (#3 §6).
        for (uint8_t i = 0; i < count_; ++i) {
            Instrument* in = inst_[i];
            if (!in) continue;
            uint8_t slot = in->id();
            if (slot < MAX_INSTRUMENTS && testAirActive_[slot] && nowMs >= testAirStopMs_[slot]) {
                if (in->air()) in->air()->stopNote();
                testAirActive_[slot] = false;
            }
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
        for (uint8_t i = 0; i < MAX_INSTRUMENTS; ++i) testAirActive_[i] = false;  // cancel all tests
        for (uint8_t i = 0; i < count_; ++i) if (inst_[i]) inst_[i]->panic(nowMs);
    }

    bool testAirActive() const {
        for (uint8_t i = 0; i < MAX_INSTRUMENTS; ++i) if (testAirActive_[i]) return true;
        return false;
    }

    // Last direct command the RT task acted on (or rejected). A client that
    // enqueued a command with a given seq can confirm execution by matching it.
    const ExecAck& lastExec() const { return lastAck_; }

private:
    void ack(const Command& c, ExecResult r) { lastAck_ = ExecAck{ c.seq, c.type, r }; }

    // Resolve a direct command's target by the instrument's STABLE id(), so
    // commands keep addressing the same physical flute even when the live set
    // is compacted (disabled instruments removed) — array index would drift.
    Instrument* byId(uint8_t id) {
        for (uint8_t i = 0; i < count_; ++i)
            if (inst_[i] && inst_[i]->id() == id) return inst_[i];
        return nullptr;
    }

    void dispatch(const Command& c, uint32_t nowMs) {
        switch (c.type) {
            case CommandType::NoteOn:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel) && inst_[i]->acceptsNote(c.a))
                        inst_[i]->noteOn(c.a, c.b, nowMs);
                break;
            case CommandType::NoteOff:
                for (uint8_t i = 0; i < count_; ++i)
                    if (inst_[i] && inst_[i]->acceptsChannel(c.channel))
                        inst_[i]->noteOff(c.a, nowMs);
                break;
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
                // absolute could never express a negative delta (#3 §7.4).
                Instrument* in = byId(c.instrument);
                if (in && in->actuator()) {
                    in->actuator()->requestPositionMm(in->actuator()->currentPositionMm() + float(c.i16));
                    ack(c, ExecResult::Accepted);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::TestActuator: {
                // absolute test position (mm) in `a`; single-instrument only (#13)
                Instrument* in = byId(c.instrument);
                if (in && in->actuator()) { in->actuator()->requestPositionMm(float(c.a)); ack(c, ExecResult::Accepted); }
                else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::TestAir: {
                Instrument* in = byId(c.instrument);
                if (in && in->air()) {
                    AirNoteRequest r; r.velocity = c.b ? c.b : 100;
                    in->air()->prepareNote(r);
                    in->air()->startNote(r);
                    // schedule an automatic stop (i16 = duration ms, default 3 s)
                    uint32_t dur = c.i16 > 0 ? (uint32_t)c.i16 : 3000u;
                    uint8_t slot = in->id();
                    if (slot < MAX_INSTRUMENTS) {
                        testAirActive_[slot] = true;
                        testAirStopMs_[slot] = nowMs + dur;
                    }
                    ack(c, ExecResult::Accepted);
                } else ack(c, ExecResult::Rejected);
                break;
            }
            case CommandType::ApplyDynamicConfig:
                // Push the newly-saved dynamic parameters into the live objects
                // so the API's "applied" claim is truthful (correction #17).
                // Config is indexed by the instrument's stable id().
                if (live_)
                    for (uint8_t i = 0; i < count_; ++i)
                        if (inst_[i] && inst_[i]->id() < live_->instrumentCount)
                            inst_[i]->applyDynamic(live_->instruments[inst_[i]->id()]);
                break;
            default: break;   // calibration handled elsewhere
        }
    }

    Instrument**       inst_ = nullptr;
    uint8_t            count_ = 0;
    CommandQueue<QN>*  q_ = nullptr;
    float              bendRangeSemis_ = 2.0f;
    const RuntimeConfig* live_ = nullptr;       // for ApplyDynamicConfig
    bool               testAirActive_[MAX_INSTRUMENTS] = {false};   // per-instrument TestAir
    uint32_t           testAirStopMs_[MAX_INSTRUMENTS] = {0};
    ExecAck            lastAck_{};                                  // last direct-command ack
};

} // namespace swc

#endif // SWC_CORE_REALTIMEENGINE_H
