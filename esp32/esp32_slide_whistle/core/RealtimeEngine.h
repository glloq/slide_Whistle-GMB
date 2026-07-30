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
        // orphans the first (reviews #16 + #11).
        for (uint8_t i = 0; i < count_ && i < MAX_INSTRUMENTS; ++i) {
            if (testAirActive_[i] && nowMs >= testAirStopMs_[i]) {
                if (inst_[i] && inst_[i]->air()) inst_[i]->air()->stopNote();
                testAirActive_[i] = false;
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

private:
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
            case CommandType::Home:
                if (c.instrument < count_ && inst_[c.instrument] && inst_[c.instrument]->actuator()) {
                    inst_[c.instrument]->actuator()->clearFault();   // allow re-home after a fault
                    inst_[c.instrument]->actuator()->requestHoming();
                }
                break;
            case CommandType::Rearm:
                // Acknowledge fault + re-arm actuator & air, then re-home so the
                // instrument is usable again after panic (review #9).
                if (c.instrument < count_ && inst_[c.instrument]) {
                    Instrument* in = inst_[c.instrument];
                    if (in->actuator()) in->actuator()->clearFault();
                    if (in->air())      in->air()->rearm();
                    if (in->actuator()) in->actuator()->requestHoming();
                }
                break;
            case CommandType::Jog:
            case CommandType::TestActuator:
                // single-instrument only — never broadcast (correction #13)
                if (c.instrument < count_ && inst_[c.instrument] && inst_[c.instrument]->actuator())
                    inst_[c.instrument]->actuator()->requestPositionMm(float(c.a));
                break;
            case CommandType::TestAir:
                if (c.instrument < count_ && inst_[c.instrument] && inst_[c.instrument]->air()) {
                    AirNoteRequest r; r.velocity = c.b ? c.b : 100;
                    inst_[c.instrument]->air()->prepareNote(r);
                    inst_[c.instrument]->air()->startNote(r);
                    // schedule an automatic stop (i16 = ms, default 3 s)
                    uint32_t dur = c.i16 > 0 ? (uint32_t)c.i16 : 3000u;
                    if (c.instrument < MAX_INSTRUMENTS) {
                        testAirActive_[c.instrument] = true;
                        testAirStopMs_[c.instrument] = nowMs + dur;
                    }
                }
                break;
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
};

} // namespace swc

#endif // SWC_CORE_REALTIMEENGINE_H
